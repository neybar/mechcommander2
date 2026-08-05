cmake_minimum_required (VERSION 3.10)
project(mclib)

set(SOURCES ${SOURCES}
    alphapalette.cpp
    bitflag.cpp
    cident.cpp
    cmponent.cpp
    color.cpp
    debugging.cpp
    err.cpp
    floathelp.cpp
    heap.cpp
    lzcomp.cpp
    lzdecomp.cpp
    mathfunc.cpp
    mouse.cpp
    msl.cpp
    paths.cpp
    pqueue.cpp
    routines.cpp
    scale.cpp
    sortlist.cpp
    tgainfo.cpp
    tgl.cpp
    timing.cpp
    txmmgr.cpp
    userinput.cpp
    vport.cpp
    weaponfx.cpp
    csvfile.cpp
    fastfile.cpp
    ffile.cpp
    file.cpp
    gamelog.cpp
    inifile.cpp
    packet.cpp
    celement.cpp
    celine.cpp
    cellip.cpp
    cepane.cpp
    cevfx.cpp
    vfx_ellipse.cpp
    vfx_map_polygon.cpp
    vfx_transform.cpp
    vfx_translatedraw.cpp
    vfxtile.cpp
    abldbug.cpp
    abldecl.cpp
    ablenv.cpp
    ablerr.cpp
    ablexec.cpp
    ablexpr.cpp
    ablrtn.cpp
    ablscan.cpp
    ablstd.cpp
    ablstmt.cpp
    ablsymt.cpp
    ablxexpr.cpp
    ablxstd.cpp
    ablxstmt.cpp
    camera.cpp
    clouds.cpp
    crater.cpp
    mapdata.cpp
    quad.cpp
    terrain.cpp
    terrtxm.cpp
    terrtxm2.cpp
    txmconv.cpp
    appear.cpp
    apprtype.cpp
    bdactor.cpp
    genactor.cpp
    gvactor.cpp
    mech3d.cpp
    soundsys.cpp
    move.cpp
    echarstring.cpp
    estring.cpp
    ewcharstring.cpp
    tacmap.cpp
    utilities.cpp
    )

include_directories(${COMMON_INCLUDE_DIRS} "./mclib" "../Editor" "../DirectX8/include" "./")
include_directories(${THIRDPARTY_INCLUDE_DIRS})

# DO NOT re-add -DBGR here. Upstream added it in 383e3c2 ("fixed paint scheme
# BGR define was missing", 2017-09-29) but the byte order it corrects for was
# ALREADY handled ten months earlier in d0ce5f4, which makes gos_LockTexture /
# gos_UnLockTexture hand CPU code 0xAARRGGBB on both backends. Defining BGR
# therefore double-corrects: it R<->B-inverts every mech and vehicle team
# colour -- whatever BaseColor/Highlightcolor are set to, the lance renders as
# their R/B swap -- and it breaks the getPaintScheme/setPaintScheme round-trip,
# which is what produced the task-4 team-colour flip.
# See ENGINEERING_LOG 2026-07-31.
add_library(mclib ${SOURCES})

# cat MCLib.vcproj | grep ".cpp" | perl -pe 's/.+\"(\w+\.cpp)\".+/\1/g' >> CMakeLists.txt

