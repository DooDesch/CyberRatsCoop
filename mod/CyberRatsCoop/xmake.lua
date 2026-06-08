-- xmake.lua — Cyber Rats Co-op UE4SS C++ mod (built as a cppmod inside the UE4SS tree).
--
-- This dir is junctioned into third_party/RE-UE4SS/cppmods/CyberRatsCoop and listed in
-- cppmods/xmake.lua. Build from the UE4SS ROOT so `ue4ssRoot` is set and UE4SS's custom package
-- repo (deps/third-repo, which provides a working raw_pdb) is used:
--   cd third_party/RE-UE4SS
--   xmake f -p windows -a x64 -m "Game__Shipping__Win64" -y
--   xmake build CyberRatsCoop
-- (scripts/build_mod.ps1 does this + deploys.) Output -> game ue4ss/Mods/CyberRatsCoop/dlls/main.dll

local projectName = "CyberRatsCoop"

target(projectName)
    add_rules("ue4ss.mod")
    add_includedirs(".")
    add_files("dllmain.cpp", "Transport/UdpTransport.cpp")
    -- Winsock for the UDP transport (Steam GNS added in M7).
    add_syslinks("ws2_32")
    set_languages("cxx20")
