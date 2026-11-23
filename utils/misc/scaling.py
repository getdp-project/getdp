#!/usr/bin/env python3
# GetDP - Copyright (C) 1997-2026 P. Dular and C. Geuzaine, University of Liege
#
# See the LICENSE.txt file for license information. Please report all
# issues on https://gitlab.onelab.info/getdp/getdp/issues.
"""
Scaling benchmark of GetDP: runs tests/scaling.pro on several numbers of MPI
ranks and prints, for each run, the time spent in each phase of the computation
and the memory used by the ranks.

  python3 scaling.py --ranks 1,2,4,8 --space 1 -n 20

With --mode weak (the default) the mesh grows with the number of ranks, so
that each rank keeps about the same number of elements (6 n^3): the times and
the memory per rank should then stay constant. With --mode strong all the runs
use the same mesh (6 n^3 elements): the times should decrease like 1/ranks.

The linear system is solved with the conjugate gradient method preconditioned
by algebraic multigrid (hypre: BoomerAMG for the scalar problem, AMS for the
vector problem), so that the solver does not hide the rest; other PETSc
options can be given with --solver-options.

The phases are those of the "Cumulative performance summary" that getdp
prints with -cpu, the memory is the one of its last message.
"""

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

# (for BoomerAMG in 3D: HMIS coarsening, extended+i interpolation with at most
# 4 entries per row, one level of aggressive coarsening and a strength
# threshold of 0.5 give cheaper coarse levels than the defaults of PETSc, for
# a few more iterations)
SOLVER_OPTIONS = {
    0: '-ksp_type cg -pc_type hypre -pc_hypre_type boomeramg -ksp_rtol 1e-8 '
       '-pc_hypre_boomeramg_coarsen_type HMIS '
       '-pc_hypre_boomeramg_interp_type ext+i -pc_hypre_boomeramg_P_max 4 '
       '-pc_hypre_boomeramg_agg_nl 1 -pc_hypre_boomeramg_strong_threshold 0.5',
    1: '-ksp_type cg -pc_type hypre -pc_hypre_type ams -ksp_rtol 1e-8',
}

# columns of the table: title and names of the operations in the summary
PHASES = [
    ('mesh', ['ReadMeshFile']),
    ('pre-pro.', ['Pre-Processing']),
    ('assembly', ['Generate[A]']),
    ('solve', ['Solve[A]']),
    ('integral', ['PostOperation[Energy]']),
    ('field map', ['PostOperation[FieldMap]']),
    ('points', ['PostOperation[Points]']),
]

_ANSI = re.compile(r'\x1b\[[0-9;]*m')
_SUMMARY = re.compile(r'^\s+(\S.*?)\s+(\d+) calls, CPU = ([\d.e+-]+)s, '
                      r'Wall = ([\d.e+-]+)s')
_STOPPED = re.compile(r'Stopped \(.*?Wall = ([\d.e+-]+)s.*?Mem = ([\d.e+-]+)MB'
                      r'(?: \[([\d.e+-]+)MB,([\d.e+-]+)MB\])?')
_ITERATIONS = re.compile(r'iterations (\d+)')
_UNKNOWNS = re.compile(r'System \d+/\d+ *: (\d+) Dofs')


def parse_log(text):
    """Return what a run printed: the wall time of each operation, the total
    wall time, the memory (total, smallest and largest of the ranks, in MB),
    the number of iterations of the linear solver and of unknowns."""
    result = {'wall': {}, 'errors': 0}
    for line in _ANSI.sub('', text).split('\n'):
        if line.startswith('Error'):
            result['errors'] += 1
        m = _SUMMARY.match(line)
        if m:
            result['wall'][m.group(1)] = float(m.group(4))
        m = _STOPPED.search(line)
        if m:
            result['total'] = float(m.group(1))
            total = float(m.group(2))
            result['memory'] = (total, float(m.group(3) or total),
                                float(m.group(4) or total))
        m = _ITERATIONS.search(line)
        if m and 'Linear' in line:
            result['iterations'] = int(m.group(1))
        m = _UNKNOWNS.search(line)
        if m:
            result['unknowns'] = int(m.group(1))
    return result


def run(cmd, cwd, log):
    print('  ' + ' '.join(shlex.quote(c) for c in cmd), flush=True)
    with open(os.path.join(cwd, log), 'w') as f:
        return subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT,
                              stdin=subprocess.DEVNULL).returncode


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    tests = os.path.join(here, '..', '..', 'tests')
    parser = argparse.ArgumentParser(
        description='Scaling benchmark of GetDP (see tests/scaling.pro).')
    parser.add_argument('--getdp', default='getdp', help='getdp executable')
    parser.add_argument('--gmsh', default='gmsh', help='gmsh executable')
    parser.add_argument('--mpiexec', default='mpiexec',
                        help='MPI launcher (run as "mpiexec -n ranks ...")')
    parser.add_argument('--ranks', default='1,2,4,8',
                        help='comma-separated numbers of MPI ranks')
    parser.add_argument('--mode', choices=['weak', 'strong'], default='weak',
                        help='weak: 6 n^3 elements per rank; strong: 6 n^3 '
                             'elements in all the runs')
    parser.add_argument('-n', type=int, default=20,
                        help='number of elements along a side of the cube '
                             '(per rank in weak mode)')
    parser.add_argument('--space', type=int, choices=[0, 1], default=0,
                        help='0: scalar problem (nodal unknowns), 1: vector '
                             'problem (edge unknowns)')
    parser.add_argument('--solver-options', default=None,
                        help='PETSc options of the linear solver (default: '
                             'CG with BoomerAMG or AMS)')
    parser.add_argument('--getdp-options', default='',
                        help='other options for getdp (e.g. "-all-dofs")')
    parser.add_argument('--partition', action='store_true',
                        help='partition the mesh with Gmsh instead of GetDP')
    parser.add_argument('--split', action='store_true',
                        help='partition the mesh with Gmsh in one file for '
                             'each rank, with its ghost cells and the entities '
                             'of the model it needs, so that each rank reads '
                             'its own file')
    parser.add_argument('--workdir', default=None,
                        help='directory of the runs, kept (default: a '
                             'temporary directory, removed at the end)')
    args = parser.parse_args()

    ranks = [int(x) for x in args.ranks.split(',') if x.strip()]
    solver = args.solver_options
    if solver is None:
        solver = SOLVER_OPTIONS[args.space]
    workdir = args.workdir or tempfile.mkdtemp(prefix='getdp_scaling_')
    os.makedirs(workdir, exist_ok=True)

    rows = []
    for np in ranks:
        n = args.n
        if args.mode == 'weak':
            n = max(1, round(args.n * np ** (1. / 3.)))
        d = os.path.join(workdir, 'np{}'.format(np))
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        for name in ('scaling.geo', 'scaling.pro'):
            shutil.copy(os.path.join(tests, name), d)
        print('{} rank(s), {} elements:'.format(np, 6 * n ** 3), flush=True)
        cmd = [args.gmsh, 'scaling.geo', '-3', '-bin', '-setnumber', 'N',
               str(n), '-v', '2']
        if args.split and np > 1:
            cmd += ['-part', str(np), '-part_split', '-part_ghosts',
                    '-setnumber', 'Mesh.PartitionSplitLocalBrep', '1']
        elif args.partition and np > 1:
            cmd += ['-part', str(np)]
        if run(cmd, d, 'gmsh.log'):
            print('  gmsh failed, see {}'.format(os.path.join(d, 'gmsh.log')))
            continue
        cmd = [args.mpiexec, '-n', str(np), args.getdp, 'scaling.pro',
               '-setnumber', 'Space', str(args.space), '-solve', 'Solve',
               '-cpu', '-bin', '-v', '3', '-ksp_converged_reason']
        cmd += shlex.split(solver) + shlex.split(args.getdp_options)
        start = time.monotonic()
        rc = run(cmd, d, 'getdp.log')
        elapsed = time.monotonic() - start
        with open(os.path.join(d, 'getdp.log'), errors='replace') as f:
            result = parse_log(f.read())
        try:
            with open(os.path.join(d, 'energy.txt')) as f:
                result['energy'] = float(f.read().split()[-1])
        except (OSError, ValueError, IndexError):
            result['energy'] = float('nan')
        if rc or result['errors'] or 'total' not in result:
            print('  getdp failed, see {}'.format(
                os.path.join(d, 'getdp.log')))
            continue
        result.update(np=np, elements=6 * n ** 3, elapsed=elapsed)
        rows.append(result)

    if not rows:
        return 1
    title = ['ranks', 'elements', 'unknowns', 'iter.', 'total'] + \
        [p[0] for p in PHASES] + ['other', 'MB/rank (max)', 'MB (all)',
                                  'efficiency', 'energy']
    print()
    print('| ' + ' | '.join(title) + ' |')
    print('|' + '---|' * len(title))
    first = rows[0]
    for r in rows:
        phases = [sum(r['wall'].get(name, 0.) for name in names)
                  for _, names in PHASES]
        if args.mode == 'weak':  # the same time with more ranks
            efficiency = first['total'] / r['total']
        else:  # a time divided by the number of ranks
            efficiency = first['total'] * first['np'] / (r['total'] * r['np'])
        cells = [str(r['np']), str(r['elements']),
                 str(r.get('unknowns', '?')), str(r.get('iterations', '?')),
                 '{:.2f}'.format(r['total'])]
        cells += ['{:.2f}'.format(x) for x in phases]
        cells += ['{:.2f}'.format(r['total'] - sum(phases)),
                  '{:.0f}'.format(r['memory'][2]),
                  '{:.0f}'.format(r['memory'][0]),
                  '{:.0f}%'.format(100 * efficiency),
                  '{:.10g}'.format(r['energy'])]
        print('| ' + ' | '.join(cells) + ' |')
    print()
    print('wall times in seconds; {} scaling, {} problem, solver options: {}'
          .format(args.mode, 'vector' if args.space else 'scalar', solver))
    if args.workdir is None:
        shutil.rmtree(workdir, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
