#!/bin/sh

# to create a tutorial screenshot on macOS:
#
# > cd tutorials
# > gmsh 01-Electrostatics/microstrip.pro &
# > screencapture -w 01-Electrostatics.png

pandoc -o tutorial.texi --shift-heading-level-by=0 ../../tutorials/README.md
sed -i '' -e '/^@node Top$/d; /^@top Top$/d' tutorial.texi
sed -i '' -e '/^@node GetDP Tutorial$/d; /^@chapter GetDP Tutorial$/d' tutorial.texi
sed -i '' -e '/^@menu$/d; /^* GetDP Tutorial::$/d; /^@end menu$/d' tutorial.texi
sed -i '' -e '/^* Further reading::$/d' tutorial.texi
sed -i '' -e '/^@node Further reading$/d' tutorial.texi
sed -i '' -e 's/@section Further reading/@unnumberedsec Further reading/g' tutorial.texi

tuto() {
    pandoc -o tmp.texi --wrap=none --shift-heading-level-by=1 ../../tutorials/$1/README.md
    sed -i '' -e '/^@node Top$/d' tmp.texi
    sed -i '' -e 's/^@top Top$/@page/g' tmp.texi
    sed -i '' -e 's/GetDP tutorial/Tutorial/g' tmp.texi
    sed -i '' -e "/^@menu/i\\
@center@image{images/$1,14cm,,Screenshot of tutorial $1}\\
@center See @url{https://gitlab.onelab.info/getdp/getdp/-/tree/master/tutorials/$1,tutorials/$1}.\\
" tmp.texi
    sed -i '' -e '/^@menu$/d' tmp.texi
    sed -i '' -e '/^* Features::$/d' tmp.texi
    sed -i '' -e '/^* Running the tutorial::$/d' tmp.texi
    sed -i '' -e '/^@end menu$/d' tmp.texi
    sed -i '' -e '/^@node Features$/d' tmp.texi
    sed -i '' -e '/^@node Running the tutorial$/d' tmp.texi
    echo "@subsection Contents" >> tmp.texi
    cat tmp.texi >> tutorial.texi
    rm -f tmp.texi
}

insert() {
    echo "@subheading File @file{$2}" >> tutorial.texi
    echo "@format" >> tutorial.texi
    echo "@verbatiminclude ../../tutorials/$1/$2" >> tutorial.texi
    echo "@end format" >> tutorial.texi
}

tuto 01-Electrostatics;
insert 01-Electrostatics microstrip.geo
insert 01-Electrostatics microstrip.pro

tuto 02-Thermal
insert 02-Thermal radiator_common.pro
insert 02-Thermal radiator.geo
insert 02-Thermal radiator.pro

tuto 03-Magnetostatics
insert 03-Magnetostatics electromagnet_common.pro
insert 03-Magnetostatics electromagnet.geo
insert 03-Magnetostatics electromagnet.pro

tuto 04-Magneto-quasistatics
insert 04-Magneto-quasistatics electromagnet_common.pro
insert 04-Magneto-quasistatics electromagnet.geo
insert 04-Magneto-quasistatics electromagnet.pro

tuto 05-Full_wave
insert 05-Full_wave full_wave_common.pro
insert 05-Full_wave full_wave.geo
insert 05-Full_wave full_wave.pro

tuto 06-Global_quantities
insert 06-Global_quantities microstrip.geo
insert 06-Global_quantities microstrip.pro

tuto 07-Coupled_problems
insert 07-Coupled_problems busbar_common.pro
insert 07-Coupled_problems busbar.geo
insert 07-Coupled_problems busbar.pro

tuto 08-Circuit_coupling
insert 08-Circuit_coupling circuit_common.pro
insert 08-Circuit_coupling circuit.geo
insert 08-Circuit_coupling circuit.pro

tuto 09-Template_library
insert 09-Template_library transfo_common.pro
insert 09-Template_library transfo.geo
insert 09-Template_library transfo.pro
insert 09-Template_library Lib_Magnetoquasistatics_av_2D_Cir.pro

tuto 09-Template_library/bonus
insert 09-Template_library/bonus electromagnet_common.pro
insert 09-Template_library/bonus electromagnet.geo
insert 09-Template_library/bonus electromagnet.pro

tuto 10-Gauging
insert 10-Gauging transfo_common.pro
insert 10-Gauging transfo.geo
insert 10-Gauging transfo.pro

sed -i '' -e 's|images/09-Template_library/bonus|images/09-Template_library_bonus|g' tutorial.texi
