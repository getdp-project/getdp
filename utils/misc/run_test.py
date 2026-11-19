#!/usr/bin/env python3
"""
Run a GetDP tutorial (or any gmsh + getdp pipeline) as a CTest test.

Designed to be invoked from CMake/CTest with the paths to the gmsh and getdp
executables (typically found by find_program() or coming from a build target)
and the per-test options for each:

    python run_test.py \\
        --gmsh /path/to/gmsh \\
        --getdp /path/to/getdp \\
        --workdir /path/to/tutorial/01-Electrostatics \\
        --name "tutorial 01" \\
        --gmsh-options "microstrip.geo -2" \\
        --getdp-options "microstrip.pro -solve Ele -pos Map"

The script runs gmsh (if ``--gmsh-options'' is non-empty) and then getdp (if
``--getdp-options'' is non-empty) in the working directory. It returns 0 on
success or the failing step's non-zero exit code on failure. Either step can
be skipped by passing an empty option string, so the same script handles
mesh-only, solve-only, or full pipeline tests.

The script is cross-platform (Windows, macOS, Linux). It copies the
tutorial's .geo and .pro files into a fresh temporary directory before
running, so several tests sharing the same source tutorial directory
can run concurrently without stepping on each other's output files
(.msh, .pos, .txt, .res, ...). The temp tree is named with the test
name (sanitised) and a random suffix; on success it is deleted, on
failure it is kept and its path is printed so the failure can be
inspected.

The basename of --workdir is preserved one level inside the temp tree,
so source files that reference siblings via ``..'' (e.g. the bonus
tutorial in 09-Template_library/bonus, whose .pro file includes
``../Lib_Magnetoquasistatics_av_2D_Cir.pro'') still resolve correctly
provided the dependency is brought in with --extra-files.

Output of gmsh and getdp is streamed straight through to the parent
process so CTest can capture and display it. Code-coverage support
(e.g. lcov/gcov for native builds) lives in the build system and in
the binaries themselves -- the runner does not need to do anything
special.

Example CMake usage:

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(GMSH gmsh REQUIRED)

    function(add_tutorial_test name dir gmsh_opts getdp_opts)
      add_test(NAME tutorial_${name}
        COMMAND ${Python3_EXECUTABLE} /path/to/run_test.py
          --gmsh ${GMSH}
          --getdp $<TARGET_FILE:getdp>
          --workdir ${CMAKE_SOURCE_DIR}/tutorials/${dir}
          --name "${name}"
          --gmsh-options "${gmsh_opts}"
          --getdp-options "${getdp_opts}"
      )
    endfunction()

    add_tutorial_test(electrostatics 01-Electrostatics
                      "microstrip.geo -2"
                      "microstrip.pro -solve Ele -pos Map")

For a tutorial that depends on a file outside its workdir (such as the
09-Template_library/bonus tutorial, which includes ``../Lib_*.pro''),
pass each such dependency with --extra-files (a path relative to
--workdir), and it will be copied to the temp tree at the same relative
position:

    add_test(NAME tutorial_template_library_bonus
      COMMAND ${Python3_EXECUTABLE} /path/to/run_test.py
        --gmsh ${GMSH}
        --getdp $<TARGET_FILE:getdp>
        --workdir ${CMAKE_SOURCE_DIR}/tutorials/09-Template_library/bonus
        --extra-files ../Lib_Magnetoquasistatics_av_2D_Cir.pro
        --gmsh-options "electromagnet.geo -2"
        --getdp-options "electromagnet.pro -solve Mag -pos Map")

Note: in CMake, keep each ``*-options'' value as a single quoted string. If
you set it via ``set(VAR a b c)'' (a CMake list, semicolon-joined), the
expansion will pass each token as a separate argument and only the first
will be picked up by ``--gmsh-options''.

Parallel tests: with --mpiexec and --ranks the getdp step is run once without
MPI and once per rank count in a copy of the run directory (``mpiexec -n N
getdp ...'', on the same mesh), and the numbers of all the text files that
the sequential run wrote (.txt, .dat, .pos) are compared with those of each
parallel run. The test fails if a file is missing, if two files do not hold
the same number of values or if they differ by more than --tolerance,
relative to the largest value of the file. No reference solution is stored:
the sequential run of the same executable is the reference, so the test
does not depend on the machine or on the version of the solver.

    add_test(NAME tutorial_electrostatics_mpi
      COMMAND ${Python3_EXECUTABLE} /path/to/run_test.py
        --gmsh ${GMSH}
        --getdp $<TARGET_FILE:getdp>
        --mpiexec ${MPIEXEC_EXECUTABLE} --ranks 2,3
        --workdir ${CMAKE_SOURCE_DIR}/tutorials/01-Electrostatics
        --gmsh-options "microstrip.geo -2"
        --getdp-options "microstrip.pro -solve Ele -pos Map")

The post-processing files must be written in text format for the comparison
(no ``-bin'' in the getdp options).

Future extensions:
  * Output-quantity checks (e.g. compare values from a ``Format Table''
    PostOperation against expected ranges). Add a ``--check'' option that
    accepts a spec like ``file:column:expected:tolerance'' and validate it
    after the getdp step.
  * Per-step timeouts via ``subprocess.run(..., timeout=...)''.
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


def _split_options(s):
    """Split a shell-style options string into a list of arguments.

    Uses POSIX-style splitting on all platforms so quoting and escaping
    behave the same everywhere. Paths containing backslashes should be
    written with forward slashes or with escaped backslashes if they appear
    inside an option string; in the typical CMake usage, paths are passed
    via ``--workdir'' and the option strings only carry relative filenames
    plus flags, so this rarely matters.
    """
    return shlex.split(s) if s else []


def _format_cmd(cmd):
    """Return a shell-quoted representation of cmd for human-readable logging."""
    return ' '.join(shlex.quote(c) for c in cmd)


def _sanitize_name(name):
    """Return ``name'' reduced to characters safe in a directory name."""
    cleaned = re.sub(r'[^A-Za-z0-9._-]+', '_', name or '').strip('_')
    return cleaned or 'test'


def setup_temp_workdir(source_workdir, extra_files, name):
    """Create a fresh temp tree containing the tutorial's .geo/.pro files.

    All top-level .geo and .pro files of ``source_workdir'' are copied into
    a directory inside the temp tree whose basename matches
    ``source_workdir'' -- so relative paths like ``../Lib.pro'' (used by the
    bonus tutorial) resolve to the same place as in the source tree, once
    the relevant parent file is brought in via ``extra_files''.

    Each entry in ``extra_files'' is a path relative to ``source_workdir''
    (e.g. ``../shared.pro''); its target inside the temp tree is computed
    by applying the same relative path to the temp run dir.

    Returns ``(temp_root, run_dir)'': ``temp_root'' is the outermost
    temporary directory to remove later, and ``run_dir'' is the cwd to
    pass to gmsh/getdp.
    """
    source_workdir = os.path.abspath(source_workdir)
    basename = os.path.basename(source_workdir.rstrip(os.sep)) or 'work'
    prefix = 'getdp_{}_'.format(_sanitize_name(name))
    temp_root = tempfile.mkdtemp(prefix=prefix)
    run_dir = os.path.join(temp_root, basename)
    os.makedirs(run_dir)

    # Copy top-level .geo / .pro files from the source workdir.
    for entry in sorted(os.listdir(source_workdir)):
        src = os.path.join(source_workdir, entry)
        if os.path.isfile(src) and entry.lower().endswith(('.geo', '.pro')):
            shutil.copy2(src, os.path.join(run_dir, entry))

    # Copy each extra file, preserving its position relative to run_dir so
    # that relative includes like ``../Lib.pro'' still resolve.
    for rel in extra_files:
        # Source path: resolve ``rel'' against the original source_workdir.
        src = os.path.normpath(os.path.join(source_workdir, rel))
        if not os.path.exists(src):
            raise FileNotFoundError(
                'extra file not found: {!r} (resolved to {})'.format(rel, src))
        # Destination: the same relative path under the temp run_dir, then
        # check that the result stays inside temp_root (defense-in-depth
        # against an extra-file path like ``../../../etc/passwd'').
        dst = os.path.normpath(os.path.join(run_dir, rel))
        try:
            common = os.path.commonpath([os.path.abspath(dst), temp_root])
        except ValueError:  # different drives on Windows
            common = ''
        if common != temp_root:
            raise ValueError(
                'extra file path {!r} resolves outside the temp tree'
                .format(rel))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.isdir(src):
            shutil.copytree(src, dst)
        else:
            shutil.copy2(src, dst)

    return temp_root, run_dir


_NUMBER = re.compile(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')


def read_numbers(path):
    """Return the list of the numbers of a text file, or None if it is binary.

    The values of a mesh-based Gmsh view ($ElementNodeData sections) are
    returned sorted by view, time step and element, without the mesh: in
    parallel each rank writes the section of its own elements, so they do not
    come in the order of the sequential file.
    """
    with open(path, 'rb') as f:
        raw = f.read()
    if b'\0' in raw:
        return None
    lines = raw.decode('utf-8', errors='replace').split('\n')
    if '$ElementNodeData' not in lines:
        return [float(x) for x in _NUMBER.findall('\n'.join(lines))]
    values = {}
    i = 0
    while i < len(lines):
        if lines[i] != '$ElementNodeData':
            i += 1
            continue
        nb_strings = int(lines[i + 1])
        view = lines[i + 2] if nb_strings else ''
        i += 2 + nb_strings
        i += 1 + int(lines[i])  # real tags (the time)
        nb_tags = int(lines[i])
        tags = [int(x) for x in lines[i + 1:i + 1 + nb_tags]]
        i += 1 + nb_tags
        step, nb_elements = tags[0], tags[2]
        for line in lines[i:i + nb_elements]:
            words = line.split()
            values[(view, step, int(words[0]))] = [float(x) for x in words[2:]]
        i += nb_elements
    return [x for key in sorted(values) for x in values[key]]


def compare_dirs(reference, other, tolerance):
    """Compare the numbers of the text result files of two run directories.

    Returns the number of files that are missing or differ by more than
    ``tolerance'' (relative to the largest value of the reference file).
    """
    nb_files = nb_failed = 0
    for root, _, files in sorted(os.walk(reference)):
        for name in sorted(files):
            if not name.lower().endswith(('.txt', '.dat', '.pos')):
                continue
            ref = os.path.join(root, name)
            rel = os.path.relpath(ref, reference)
            a = read_numbers(ref)
            if a is None:
                print('[compare] {}: binary file, skipped'.format(rel))
                continue
            nb_files += 1
            if not os.path.exists(os.path.join(other, rel)):
                print('[compare] {}: FAILED, missing'.format(rel))
                nb_failed += 1
                continue
            b = read_numbers(os.path.join(other, rel))
            if b is None or len(a) != len(b):
                print('[compare] {}: FAILED, {} values instead of {}'.format(
                    rel, 'binary file' if b is None else len(b), len(a)))
                nb_failed += 1
                continue
            scale = max([abs(x) for x in a] + [1e-300])
            diff = max([abs(x - y) for x, y in zip(a, b)] + [0.]) / scale
            ok = diff <= tolerance  # False for a NaN
            print('[compare] {}: {} values, max relative difference {:.1e}{}'
                  .format(rel, len(a), diff, '' if ok else ' FAILED'))
            if not ok:
                nb_failed += 1
    if not nb_files:
        print('[compare] FAILED, no result file to compare')
        nb_failed = 1
    sys.stdout.flush()
    return nb_failed


def mpi_environment():
    """Environment for mpiexec in a test: Open MPI refuses by default to run
    as root (as in a Docker container) or with more ranks than cores."""
    env = dict(os.environ)
    env.setdefault('OMPI_ALLOW_RUN_AS_ROOT', '1')
    env.setdefault('OMPI_ALLOW_RUN_AS_ROOT_CONFIRM', '1')
    env.setdefault('OMPI_MCA_rmaps_base_oversubscribe', 'true')  # Open MPI 4
    env.setdefault('PRTE_MCA_rmaps_default_mapping_policy',
                   ':oversubscribe')  # Open MPI 5
    return env


def run_step(label, executable, args, workdir, env=None):
    """Run one external command (gmsh or getdp). Returns its exit code.

    stdout and stderr are inherited from the parent, so output appears
    directly in the CTest log. A clearly formatted header makes the log
    easy to scan when several tests run back-to-back.
    """
    cmd = [executable] + args
    print('-' * 70, flush=True)
    print('[{}] cwd: {}'.format(label, workdir), flush=True)
    print('[{}] cmd: {}'.format(label, _format_cmd(cmd)), flush=True)
    start = time.monotonic()
    try:
        proc = subprocess.run(cmd, cwd=workdir, check=False, env=env)
        rc = proc.returncode
    except FileNotFoundError:
        print('[{}] ERROR: executable not found: {}'.format(
            label, executable), flush=True)
        return 127
    except OSError as exc:
        print('[{}] ERROR: {}'.format(label, exc), flush=True)
        return 1
    elapsed = time.monotonic() - start
    status = 'OK' if rc == 0 else 'FAILED (exit {})'.format(rc)
    print('[{}] {} in {:.2f}s'.format(label, status, elapsed), flush=True)
    return rc


def main(argv=None):
    parser = argparse.ArgumentParser(
        description='Run a GetDP tutorial (gmsh + getdp) as a CTest test.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Returns 0 if every requested step succeeds, or the failing '
               'step\'s exit code otherwise.')
    parser.add_argument(
        '--gmsh', required=True, metavar='PATH',
        help='Path to the gmsh executable (typically found by CMake).')
    parser.add_argument(
        '--getdp', required=True, metavar='PATH',
        help='Path to the getdp executable (typically found by CMake).')
    parser.add_argument(
        '--workdir', metavar='DIR', default=None,
        help='Directory in which gmsh and getdp are run '
             '(default: current directory).')
    parser.add_argument(
        '--name', metavar='NAME', default='',
        help='Human-readable test name shown in log output.')
    parser.add_argument(
        '--gmsh-options', metavar='STRING', default='',
        help='Shell-style string of arguments for gmsh, split exactly as a '
             'shell would (e.g. "microstrip.geo -2"). Empty (the default) '
             'skips the gmsh step.')
    parser.add_argument(
        '--getdp-options', metavar='STRING', default='',
        help='Shell-style string of arguments for getdp (e.g. '
             '"microstrip.pro -solve Ele -pos Map"). Empty (the default) '
             'skips the getdp step.')
    parser.add_argument(
        '--extra-files', metavar='PATH', action='append', default=[],
        help='Extra file or directory to copy into the temp working tree '
             '(in addition to the .geo/.pro files in --workdir). PATH is '
             'interpreted relative to --workdir and copied to the same '
             'position relative to the temp run directory, so includes like '
             '"../Lib.pro" resolve correctly. May be given multiple times.')
    parser.add_argument(
        '--mpiexec', metavar='PATH', default='',
        help='Path to mpiexec: with --ranks, also run getdp in parallel and '
             'compare its results with those of the run without MPI.')
    parser.add_argument(
        '--ranks', metavar='LIST', default='',
        help='Comma-separated numbers of MPI ranks of the parallel runs '
             '(e.g. "2,3").')
    parser.add_argument(
        '--tolerance', metavar='VALUE', type=float, default=1e-5,
        help='Largest accepted difference between a parallel and the '
             'sequential run, relative to the largest value of each file '
             '(default: 1e-5).')
    args = parser.parse_args(argv)
    ranks = [int(n) for n in args.ranks.split(',') if n.strip()]
    if ranks and not args.mpiexec:
        sys.stderr.write('ERROR: --ranks needs --mpiexec\n')
        return 2

    source_workdir = args.workdir or os.getcwd()
    if not os.path.isdir(source_workdir):
        sys.stderr.write(
            'ERROR: --workdir does not exist: {}\n'.format(source_workdir))
        return 2

    gmsh_args = _split_options(args.gmsh_options)
    getdp_args = _split_options(args.getdp_options)
    if not gmsh_args and not getdp_args:
        sys.stderr.write(
            'ERROR: at least one of --gmsh-options or --getdp-options '
            'must be non-empty\n')
        return 2

    label = args.name or 'test'
    print('=== {} ==='.format(label), flush=True)
    overall_start = time.monotonic()

    # Copy sources into a fresh temp directory so concurrent tests on the
    # same tutorial don't collide on .msh/.pos/.txt/.res output files.
    try:
        temp_root, run_dir = setup_temp_workdir(
            source_workdir, args.extra_files, label)
    except (FileNotFoundError, ValueError, OSError) as exc:
        sys.stderr.write('ERROR setting up temp workdir: {}\n'.format(exc))
        return 2
    print('[setup] temp run dir: {}'.format(run_dir), flush=True)

    rc = 0
    failing_step = None
    try:
        if gmsh_args:
            rc = run_step('gmsh', args.gmsh, gmsh_args, run_dir)
            if rc != 0:
                failing_step = 'gmsh'

        # The parallel runs start from a copy of the meshed run directory,
        # next to it in the temp tree (so that ``..'' includes still resolve).
        parallel_dirs = []
        if rc == 0 and getdp_args:
            for n in ranks:
                copy = os.path.join(temp_root, 'mpi{}'.format(n))
                shutil.copytree(temp_root, copy,
                                ignore=shutil.ignore_patterns('mpi[0-9]*'))
                parallel_dirs.append(
                    (n, os.path.join(copy, os.path.basename(run_dir))))

        if rc == 0 and getdp_args:
            rc = run_step('getdp', args.getdp, getdp_args, run_dir)
            if rc != 0:
                failing_step = 'getdp'

        for n, parallel_dir in parallel_dirs:
            if rc != 0:
                break
            step = 'getdp on {} ranks'.format(n)
            rc = run_step(step, args.mpiexec,
                          ['-n', str(n), args.getdp] + getdp_args,
                          parallel_dir, mpi_environment())
            if rc == 0:
                step = 'comparison of {} ranks with the run without MPI'.format(n)
                rc = compare_dirs(run_dir, parallel_dir, args.tolerance)
            if rc != 0:
                failing_step = step
    finally:
        if rc == 0:
            shutil.rmtree(temp_root, ignore_errors=True)
        else:
            # Keep the temp tree on failure so the .msh/.pos/log files are
            # available for inspection. CTest captures this line in the log.
            print('[cleanup] kept temp dir for inspection: {}'
                  .format(temp_root), flush=True)

    if rc != 0:
        print('=== {} FAILED at {} step ==='.format(label, failing_step),
              flush=True)
        return rc

    elapsed = time.monotonic() - overall_start
    print('=== {} PASSED in {:.2f}s ==='.format(label, elapsed), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
