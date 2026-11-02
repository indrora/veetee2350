#!/bin/bash
# vtdemo.sh — exercise the VeeTee terminal from a real shell.
# Run inside tools/hostshell.py (TERM=xterm-256color, 80x30).
#
# Covers: clear/home, SGR attributes, 16 + 256 indexed colours, 24-bit
# colour, DEC special graphics via tput (smacs/rmacs), and absolute
# cursor positioning.

printf '\033[2J\033[H'
printf '\033[1;33mVeeTee 2350 terminal demo\033[0m  TERM=%s  %sx%s\n\n' \
    "$TERM" "$(tput cols)" "$(tput lines)"

printf 'attrs: \033[1mbold\033[0m \033[4munderline\033[0m '
printf '\033[5mblink\033[0m \033[7mreverse\033[0m \033[9mstrike\033[0m '
printf '\033[8mhidden\033[0m(hidden)\n\n'

printf 'ansi16: '
for i in $(seq 0 15); do printf '\033[48;5;%dm  ' "$i"; done
printf '\033[0m\n'

printf '256:\n'
for row in 0 1 2 3 4 5; do
    for col in $(seq 0 35); do
        printf '\033[48;5;%dm ' $((16 + row * 36 + col))
    done
    printf '\033[0m\n'
done
printf 'grey: '
for i in $(seq 232 255); do printf '\033[48;5;%dm  ' "$i"; done
printf '\033[0m\n'

printf 'rgb:  '
for i in $(seq 0 73); do
    r=$(( i * 255 / 73 ))
    printf '\033[48;2;%d;%d;%dm ' "$r" $(( 255 - r )) 128
done
printf '\033[0m\n\n'

# DEC special graphics through terminfo's alternate charset.
smacs=$(tput smacs); rmacs=$(tput rmacs)
printf '%slqqqqqqqqwqqqqqqqqk%s\n' "$smacs" "$rmacs"
printf '%sx%s  dec   %sx%s  box   %sx%s\n' "$smacs" "$rmacs" "$smacs" \
    "$rmacs" "$smacs" "$rmacs"
printf '%smqqqqqqqqvqqqqqqqqj%s\n' "$smacs" "$rmacs"

# Absolute positioning: a marker in the right-hand corner area.
printf '\033[s\033[20;60H\033[1;32m<- CUP 20;60\033[0m\033[u\n'
