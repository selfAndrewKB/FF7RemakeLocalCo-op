param(
    [Parameter(Mandatory)] [string[]]$Address,   # IDA addresses (image base 0x140000000), e.g. 0x141686C00
    [int]$Count = 16,
    [string]$Exe = "$PSScriptRoot\..\ff7remake_.exe",
    [switch]$Pointer                              # read one 8-byte pointer (a vtable entry) instead of bytes
)
# Reads bytes at IDA addresses from the executable on disk (file offset from the section table), for the
# expected-bytes checks the mod makes at startup. Code and vtables are not relocated by the loader in
# ways that change these reads, except absolute pointers, which are printed as IDA addresses.
$ErrorActionPreference = 'Stop'
$bytes = [IO.File]::ReadAllBytes((Resolve-Path $Exe))
$pe = [BitConverter]::ToInt32($bytes, 0x3C)
$sections = [BitConverter]::ToUInt16($bytes, $pe + 6)
$optSize = [BitConverter]::ToUInt16($bytes, $pe + 20)
$imageBase = [BitConverter]::ToUInt64($bytes, $pe + 24 + 24)
$table = $pe + 24 + $optSize
foreach ($a in $Address) {
    # Unquoted 0x literals reach a [string[]] parameter as decimal text, so accept both forms.
    $value = if ($a -match '^0x') { [Convert]::ToUInt64($a.Substring(2), 16) } else { [UInt64]$a }
    $a = '0x{0:X}' -f $value
    $rva = $value - $imageBase
    $offset = $null
    for ($i = 0; $i -lt $sections; $i++) {
        $s = $table + 40 * $i
        $va = [BitConverter]::ToUInt32($bytes, $s + 12); $raw = [BitConverter]::ToUInt32($bytes, $s + 16)
        $ptr = [BitConverter]::ToUInt32($bytes, $s + 20)
        if ($rva -ge $va -and $rva -lt $va + $raw) { $offset = $ptr + ($rva - $va); break }
    }
    if ($null -eq $offset) { "$a : not in a file-backed section"; continue }
    if ($Pointer) { "{0} : 0x{1:X}" -f $a, [BitConverter]::ToUInt64($bytes, [int]$offset); continue }
    "{0} : {1}" -f $a, (($bytes[[int]$offset..([int]$offset + $Count - 1)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' ')
}
