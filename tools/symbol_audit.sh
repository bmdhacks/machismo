#!/bin/bash
# Symbol audit: compare Mach-O and native .so symbol spaces for a trampoline library.
#
# Usage: ./tools/symbol_audit.sh <macho_binary> <prefix> <native_so>
# Example: ./tools/symbol_audit.sh ../necrodancer/.../NecroDancer __ZN4bgfx ./build-bgfx/libbgfx-shared.so
#
# Uses the same combinatorial y↔m substitution as the runtime trampoline code,
# and categorizes unmatched symbols the same way (renderer-specific, DXBC, etc.)

set -e

MACHO="$1"
PREFIX="$2"
NATIVE="$3"

if [ -z "$MACHO" ] || [ -z "$PREFIX" ] || [ -z "$NATIVE" ]; then
    echo "Usage: $0 <macho_binary> <prefix> <native_so>"
    exit 1
fi

if [ ! -f "$MACHO" ]; then echo "Error: $MACHO not found"; exit 1; fi
if [ ! -f "$NATIVE" ]; then echo "Error: $NATIVE not found"; exit 1; fi

# Extract Mach-O global text symbols matching prefix (strip leading _)
macho_syms=$(llvm-nm "$MACHO" 2>/dev/null | grep " T ${PREFIX}" | awk '{print $3}' | sed 's/^_//' | sort -u)
macho_count=$(echo "$macho_syms" | grep -c . || true)

# Extract native .so dynamic symbols into a temp file for fast grep
native_file=$(mktemp)
nm -D --defined-only "$NATIVE" 2>/dev/null | awk '{print $3}' | sort -u > "$native_file"
native_count=$(wc -l < "$native_file")

# Exact matches
exact=$(comm -12 <(echo "$macho_syms") "$native_file" | wc -l)

# Unmatched in Mach-O
unmatched=$(comm -23 <(echo "$macho_syms") "$native_file")

# Combinatorial y→m substitution (matches runtime try_mangling_variants).
# For each unmatched symbol with N 'y' characters, try all 2^N-1 non-empty
# subsets of y→m replacement. This correctly handles names like "Memory"
# that contain 'y' in length-prefixed components.
try_mangling_match() {
    local sym="$1"

    # Collect positions of 'y' characters
    local positions=()
    for ((i=0; i<${#sym}; i++)); do
        if [ "${sym:$i:1}" = "y" ]; then
            positions+=("$i")
        fi
    done

    local ny=${#positions[@]}
    if [ "$ny" -eq 0 ] || [ "$ny" -gt 16 ]; then
        return 1
    fi

    # Try all non-empty subsets
    local total=$((1 << ny))
    for ((mask=1; mask<total; mask++)); do
        local buf="$sym"
        for ((j=0; j<ny; j++)); do
            if (( mask & (1 << j) )); then
                local pos=${positions[$j]}
                buf="${buf:0:$pos}m${buf:$((pos+1))}"
            fi
        done
        if grep -qFx "$buf" "$native_file"; then
            echo "$buf"
            return 0
        fi
    done
    return 1
}

mangling_fixed=0
renderer_specific=0
dxbc_specific=0
platform_abi=0
truly_missing=0

mangling_list=""
renderer_list=""
dxbc_list=""
platform_list=""
missing_list=""

while IFS= read -r sym; do
    [ -z "$sym" ] && continue

    # Try combinatorial y→m substitution
    match=$(try_mangling_match "$sym" 2>/dev/null) && {
        mangling_fixed=$((mangling_fixed + 1))
        mangling_list="${mangling_list}    ${sym} -> ${match}\n"
        continue
    }

    # Categorize the unmatched symbol
    if echo "$sym" | grep -qE '2vk|2d3d|3mtl|3agc|3gnm|3nvn|Metal|Vulkan|nvn_size'; then
        renderer_specific=$((renderer_specific + 1))
        renderer_list="${renderer_list}    ${sym}\n"
    elif echo "$sym" | grep -qE 'Dxbc|Dx9bc|dxbc'; then
        dxbc_specific=$((dxbc_specific + 1))
        dxbc_list="${dxbc_list}    ${sym}\n"
    elif echo "$sym" | grep -qE '__va_list|va_list_tag|Pc$'; then
        platform_abi=$((platform_abi + 1))
        platform_list="${platform_list}    ${sym}\n"
    else
        truly_missing=$((truly_missing + 1))
        missing_list="${missing_list}    ${sym}\n"
    fi
done <<< "$unmatched"

coverage=$((exact + mangling_fixed))
trapped=$((renderer_specific + dxbc_specific + platform_abi + truly_missing))
if [ "$macho_count" -gt 0 ]; then
    pct=$((coverage * 100 / macho_count))
else
    pct=0
fi

echo "=== Symbol Audit: ${PREFIX} ==="
echo "  Mach-O symbols (global text): $macho_count"
echo "  Native .so exports:           $native_count"
echo "  ---"
echo "  Exact matches:                $exact"
echo "  Mangling-fixed (y↔m):         $mangling_fixed"
echo "  Trapped (no native match):    $trapped"
echo "    Renderer-specific (vk/d3d/mtl/nvn): $renderer_specific"
echo "    DXBC shader parsers:                $dxbc_specific"
echo "    Platform ABI (va_list/char*):       $platform_abi"
echo "    Truly missing:                      $truly_missing"
echo "  ---"
echo "  Coverage:                     $coverage/$macho_count ($pct%)"

if [ -n "$mangling_list" ]; then
    echo ""
    echo "  Mangling fixes:"
    echo -e "$mangling_list" | head -20
fi

if [ -n "$missing_list" ]; then
    echo ""
    echo "  Truly missing (will abort if called):"
    echo -e "$missing_list"
fi

rm -f "$native_file"
