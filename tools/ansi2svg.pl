#!/usr/bin/perl
# Renders terminal output (UTF-8 text with ANSI SGR colour codes) as an SVG
# "screenshot": a dark terminal window with a monospace font. Used to produce
# the images in docs/ from the real program output, so that the README always
# shows what the tool actually prints.
#
#   LEDGER_STYLE=fancy ledger demo < script.sql | perl tools/ansi2svg.pl > docs/out.svg
#
# Supported SGR codes: 0 reset, 1 bold, 2 dim, 3 italic, 30-37 / 90-97 colours,
# 38;5;N (256-colour foreground). Anything else is ignored.
use strict;
use warnings;
use utf8;
binmode STDIN, ':encoding(UTF-8)';
binmode STDOUT, ':encoding(UTF-8)';

my $title = $ARGV[0] // 'ledger';

# Terminal palette (One Dark-ish), foreground on a dark background.
my %basic = (
    30 => '#3b4048', 31 => '#e06c75', 32 => '#98c379', 33 => '#e5c07b',
    34 => '#61afef', 35 => '#c678dd', 36 => '#56b6c2', 37 => '#d7dae0',
    90 => '#6b717d', 91 => '#e06c75', 92 => '#98c379', 93 => '#e5c07b',
    94 => '#61afef', 95 => '#c678dd', 96 => '#56b6c2', 97 => '#ffffff',
);

# 256-colour cube for 38;5;N (only the 6x6x6 cube and greys are needed).
sub xterm256 {
    my ($n) = @_;
    return $basic{30 + $n} // '#d7dae0' if $n < 8;
    return $basic{90 + $n - 8} // '#ffffff' if $n < 16;
    if ($n < 232) {
        my $i = $n - 16;
        my @lv = (0, 95, 135, 175, 215, 255);
        my ($r, $g, $b) = ($lv[int($i / 36)], $lv[int($i / 6) % 6], $lv[$i % 6]);
        return sprintf('#%02x%02x%02x', $r, $g, $b);
    }
    my $v = 8 + ($n - 232) * 10;
    return sprintf('#%02x%02x%02x', $v, $v, $v);
}

my $fg_default = '#d7dae0';
my @lines;          # each line: list of [text, fg, bold, dim, italic]
my ($fg, $bold, $dim, $italic) = ($fg_default, 0, 0, 0);
my $maxcols = 0;

for my $raw (<STDIN>) {
    $raw =~ s/\r?\n\z//;
    my @spans;
    my $cols = 0;
    while ($raw ne '') {
        if ($raw =~ s/\A\e\[([0-9;]*)m//) {
            my @codes = split /;/, ($1 eq '' ? '0' : $1);
            while (@codes) {
                my $c = shift @codes;
                if ($c == 0) { ($fg, $bold, $dim, $italic) = ($fg_default, 0, 0, 0); }
                elsif ($c == 1) { $bold = 1 }
                elsif ($c == 2) { $dim = 1 }
                elsif ($c == 3) { $italic = 1 }
                elsif ($c == 38 && @codes >= 2 && $codes[0] == 5) { shift @codes; $fg = xterm256(shift @codes); }
                elsif (exists $basic{$c}) { $fg = $basic{$c} }
            }
        } elsif ($raw =~ s/\A([^\e]+)//) {
            my $text = $1;
            push @spans, [$text, $fg, $bold, $dim, $italic];
            $cols += length $text;
        } else {
            $raw =~ s/\A.//;  # stray escape byte
        }
    }
    push @lines, \@spans;
    $maxcols = $cols if $cols > $maxcols;
}

my $char_w = 8.4;   # px per column at 14px monospace
my $line_h = 20;
my $pad = 16;
my $width = int($pad * 2 + $maxcols * $char_w + 8);
my $height = $pad * 2 + 28 + @lines * $line_h;

sub esc {
    my ($s) = @_;
    $s =~ s/&/&amp;/g; $s =~ s/</&lt;/g; $s =~ s/>/&gt;/g;
    return $s;
}

print qq{<svg xmlns="http://www.w3.org/2000/svg" width="$width" height="$height" font-family="Cascadia Code, JetBrains Mono, Fira Code, Consolas, DejaVu Sans Mono, monospace" font-size="14">\n};
print qq{  <rect width="100%" height="100%" rx="10" fill="#282c34"/>\n};
# Window chrome: three dots and a title.
print qq{  <circle cx="22" cy="20" r="6" fill="#ff5f57"/><circle cx="42" cy="20" r="6" fill="#febc2e"/><circle cx="62" cy="20" r="6" fill="#28c840"/>\n};
print qq{  <text x="} . ($width / 2) . qq{" y="25" text-anchor="middle" fill="#9da5b4" font-size="12">} . esc($title) . qq{</text>\n};

my $y = $pad + 28 + 14;
for my $spans (@lines) {
    my $x = $pad;
    print qq{  <text x="$x" y="$y" xml:space="preserve">};
    for my $s (@$spans) {
        my ($text, $color, $b, $d, $i) = @$s;
        my $style = qq{fill="$color"};
        $style .= qq{ font-weight="bold"} if $b;
        $style .= qq{ opacity="0.6"} if $d;
        $style .= qq{ font-style="italic"} if $i;
        print qq{<tspan $style>} . esc($text) . qq{</tspan>};
    }
    print qq{</text>\n};
    $y += $line_h;
}
print qq{</svg>\n};
