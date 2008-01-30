#!/usr/bin/env perl
#
# Copyright (c) 2008 Cisco Systems, Inc.  All rights reserved.
#
# Dumb script to run through all the svn:ignore's in the tree and build
# build a .hgignore file for Mercurial.  Do a few trivial things to
# to try to have a few "global" files that don't need to be listed in
# every directory (i.e., reduce the total length of the .hgignore file).

use strict;

# Sanity check
die "Not in HG+SVN repository top dir"
    if (! -d ".hg" && ! -d ".svn");

# Put in some specials that we ignore everywhere
my @hgignore;
push(@hgignore, "# Automatically generated file; edits may be lost!
syntax: glob");
my @globals = qw/.libs
.deps
.svn
*.la
*.lo
*.o
*.so
*.a
.dirstamp
.DS_Store
stamp-h[1-9]
configure
config.guess
config.sub
config.log
config.status
libtool
ltmain.sh
missing
depcomp
install-sh
aclocal.m4
autom4te.cache
Makefile
Makefile.in
static-components.h
*~
*\\\#/;

# Start at the top level
process(".");

# If there's an old .hgignore, save it
if (-f ".hgignore") {
    my ($sec,$min,$hour,$mday,$mon,$year,$wday,$yday,$isdst) =
        localtime(time);
    my $new_name = sprintf(".hgignore.%04d%02d%02d-%02d%02d",
                           $year + 1900, $mon + 1, $mday, $hour, $min);
    rename(".hgignore", $new_name) || 
        die "Unable to rename old .hgignore file: $!";
}

# Write the new one
open(FILE, ">.hgignore");
print FILE join("\n", @hgignore) . "\n";
print FILE join("\n", @globals) . "\n"; 
close(FILE);
print "All done!\n";
exit(0);

#######################################################################

# DFS-oriented recursive directory search
sub process {
    my $dir = shift;

    # Look at the svn:ignore property for this directory
    my $svn_ignore = `svn pg svn:ignore $dir`;
    chomp($svn_ignore);
    if ($svn_ignore ne "") {
        print "Found svn:ignore in $dir\n";
#        my $formatted_dir = $dir;
#        if ($formatted_dir eq ".") {
#            $formatted_dir = "";
#        } else {
#            $formatted_dir =~ s/^\.\///;
#            $formatted_dir .= "/";
#        }
        
        foreach my $line (split(/\n/, $svn_ignore)) {
            chomp($line);
            $line =~ s/^\.\///;
            next
                if ($line eq "");

            # Ensure not to ignore special hg files
            next
                if ($line eq ".hgignore" || $line eq ".hgrc" || 
                    $line eq ".hg");
            # We're globally ignoring some specials already; we can
            # skip those
            my $skip = 0;
            foreach my $g (@globals) {
                if ($g eq $line) {
                    $skip = 1;
                    last;
                }
            }
            next 
                if ($skip);

            push(@hgignore, "$dir/$line");
        }
    }
        
    # Now find subdirectories in this directory
    my @entries;
    opendir(DIR, $dir) || die "Cannot open directory \"$dir\" for reading: $!";
    @entries = readdir(DIR);
    closedir DIR;

    foreach my $e (@entries) {
        # Skip special directories and sym links
        next
            if ($e eq "." || $e eq ".." || $e eq ".svn" || $e eq ".hg" ||
                -l "$dir/$e");

        # If it's a directory, analyze it
        process("$dir/$e")
            if (-d "$dir/$e");
    }
}