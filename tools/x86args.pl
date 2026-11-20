#!/usr/bin/perl
# x86args.pl ntrun.c LIBFOLDER... > x86args.h
#
# On x86 a Windows function takes its arguments off the stack when it returns (stdcall).
# So ntrun must know how many bytes of arguments each function has. The import
# libraries of the Win32 SDKs for x86 have this in the names: "__imp__CreateFileA@28".
# This script reads the names of the functions that ntrun has (the E(...) entries of
# ntrun.c) and looks them up in the .LIB files of the folders.
use strict;

my $src = shift or die "usage: x86args.pl ntrun.c LIBFOLDER...\n";
my (%have, %bytes);

open(my $s, '<', $src) or die "$src: $!\n";
while (<$s>) {
    $have{$1} = 1 while /\bE\(([A-Za-z0-9_]+)\)/g;
}
close $s;

for my $dir (@ARGV) {
    opendir(my $d, $dir) or die "$dir: $!\n";
    for my $f (sort grep { /\.lib$/i } readdir $d) {
        open(my $l, '<', "$dir/$f") or next;
        binmode $l;
        local $/;
        my $b = <$l>;
        close $l;
        # "__imp__Name@12": stdcall with 12 bytes; "__imp__Name" alone: the caller takes them off
        while ($b =~ /__imp__([A-Za-z0-9_]+)(?:@(\d+))?\0/g) {
            my ($n, $c) = ($1, defined $2 ? $2 : -1);
            next unless $have{$n};
            $bytes{$n} = $c unless exists $bytes{$n} && $bytes{$n} >= 0;
        }
    }
    closedir $d;
}

print "/* x86args.h: made by tools/x86args.pl from the import libraries of the Win32 SDKs for x86.\n";
print "   For each Windows function that ntrun has: the bytes of arguments that the function\n";
print "   takes off the stack when it returns on x86. -1: the caller takes them off (cdecl). */\n";
for my $n (sort keys %have) {
    printf "    { \"%s\", %d },\n", $n, $bytes{$n} if exists $bytes{$n};
}
my @none = grep { !exists $bytes{$_} } sort keys %have;
print "/* not in the libraries: @none */\n" if @none;
