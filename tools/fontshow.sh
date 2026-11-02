#!/bin/bash
# fontshow.sh — cycle the VeeTee through several built-in fonts, drawing
# the same sample page in each. Run inside tools/hostshell.py:
#   ./fontshow.sh [seconds-per-font] [font...]
cd "$(dirname "$0")"
hold=${1:-10}; shift
fonts=("$@")
[ ${#fonts[@]} -eq 0 ] && fonts=(notjam-clean-16 notjam-prophet-8
    notjam-prophet-11 notjam-casual-10 arbata-compact)

sample() {
	local name=$1 cols rows smacs rmacs
	cols=$(tput cols); rows=$(tput lines)
	smacs=$(tput smacs); rmacs=$(tput rmacs)
	printf '\033[1;33m%s\033[0m  %sx%s\n' "$name" "$cols" "$rows"
	printf '1234567890%.0s' 1 2 3 4 5 6 7 8; printf '\n'
	echo 'The quick brown fox jumps over the lazy dog. 0123456789'
	echo 'THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG! @#$%^&*_~|\{}[]<>'
	echo 'Latin-1: àéîõü ÀÉÎÕÜ ñç ß ¿¡ £¥€ ±°·  Cyrillic: Съешь же ещё'
	printf '%slqqqqqwqqqqqk%s  ' "$smacs" "$rmacs"
	for i in 1 2 3 4 5 6 9 10 11 12 13 14; do printf '\033[4%sm  ' $((i < 8 ? i : i - 8)); done
	printf '\033[0m\n'
	printf '%sx%s box %sx%s DEC %sx%s  ' "$smacs" "$rmacs" "$smacs" "$rmacs" "$smacs" "$rmacs"
	printf '\033[1mbold\033[0m \033[4munder\033[0m \033[7mrev\033[0m \033[9mstrike\033[0m\n'
	printf '%smqqqqqvqqqqqj%s\n' "$smacs" "$rmacs"
	# Fill the rest so the grid size is visible: one numbered line per row.
	for ((row = 9; row < rows; row++)); do
		printf '\033[3%dmrow %2d of %d \033[0m%s\n' $((row % 6 + 1)) \
		    "$row" "$rows" "...................................................."
	done
	printf 'row %d of %d $ ' "$rows" "$rows"
}

for font in "${fonts[@]}"; do
	./setfont "$font" || exit 1
	clear
	sample "$font"
	sleep "$hold"
done
