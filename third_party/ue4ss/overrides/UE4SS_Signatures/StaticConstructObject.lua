-- Custom AOB for StaticConstructObject_Internal in Cyber Rats (UnrealGame-Win64-Shipping.exe, UE 5.6).
--
-- UE4SS' built-in scanner fails ("Failed to find StaticConstructObject_Internal") on this UE5.6
-- build, which aborts the whole pattern-scan (PS scan timed out). This supplies the correct AOB.
--
-- Derived empirically: this is the function prologue (mov r11,rsp / push rbp,rbx,r14 /
-- lea rbp,[r11-0x1D8] / sub rsp,0x2C0) shared verbatim by other recent UE5 titles (Whiskerwood,
-- Tokyo Xtreme Racer). Verified to match EXACTLY ONCE in the Cyber Rats shipping exe (file offset
-- 0x15ABEC0), so it uniquely identifies the function.
--
-- File location: <game>\Engine\Binaries\Win64\ue4ss\UE4SS_Signatures\StaticConstructObject.lua

function Register()
    return "4C 8B DC 55 53 41 56 49 8D AB 28 FE FF FF 48 81 EC C0 02 00 00 48 8B"
end

function OnMatchFound(MatchAddress)
    return MatchAddress
end
