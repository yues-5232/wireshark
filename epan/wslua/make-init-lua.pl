#!/usr/bin/perl
#
# make-init-lua.pl
#
# create the init.lua file based on a template (stdin)
#
# (c) 2006, Luis E. Garcia Onatnon <luis@ontanon.org>
#
# Wireshark - Network traffic analyzer
# By Gerald Combs <gerald@wireshark.org>
# Copyright 2004 Gerald Combs
#
# SPDX-License-Identifier: GPL-2.0-or-later

use strict;

my $WSROOT = shift;

die "'$WSROOT' is not a directory" unless -d $WSROOT;

my $wtap_encaps_table = '';
my $wtap_tsprecs_table = '';
my $wtap_commenttypes_table = '';
my $ft_types_table = '';
my $frametypes_table = '';
my $wtap_rec_types_table = '';
my $wtap_presence_flags_table = '';
my $bases_table = '';
my $encodings = '';
my $expert_pi = '';
my $expert_pi_tbl = '';
my $expert_pi_severity = '';
my $expert_pi_group = '';
my $menu_groups = '';

my %replacements = %{{
    WTAP_ENCAPS => \$wtap_encaps_table,
    WTAP_TSPRECS => \$wtap_tsprecs_table,
    WTAP_COMMENTTYPES => \$wtap_commenttypes_table,
    FT_TYPES => \$ft_types_table,
    FT_FRAME_TYPES => \$frametypes_table,
    WTAP_REC_TYPES => \$wtap_rec_types_table,
    WTAP_PRESENCE_FLAGS => \$wtap_presence_flags_table,
    BASES => \$bases_table,
    ENCODINGS => \$encodings,
    EXPERT => \$expert_pi,
    EXPERT_TABLE => \$expert_pi_tbl,
    MENU_GROUPS => \$menu_groups,
}};


#
# load template
#
my $template = '';
my $template_filename = shift;

open TEMPLATE, "< $template_filename" or die "could not open '$template_filename':  $!";
$template .= $_ while(<TEMPLATE>);
close TEMPLATE;

#
# Extract values from wiretap/wtap.h:
#
#   WTAP_FILE_  values
#	WTAP_ENCAP_ values
#   WTAP_HAS_   values
#

$wtap_encaps_table = "-- Wiretap encapsulations XXX\nwtap_encaps = {\n";
$wtap_tsprecs_table = "-- Wiretap timestamp precision types\nwtap_tsprecs = {\n";
$wtap_commenttypes_table = "-- Wiretap file comment types\nwtap_comments = {\n";
$wtap_rec_types_table = "-- Wiretap record_types\nwtap_rec_types = {\n";
$wtap_presence_flags_table = "-- Wiretap presence flags\nwtap_presence_flags = {\n";

open WTAP_H, "< $WSROOT/wiretap/wtap.h" or die "cannot open '$WSROOT/wiretap/wtap.h':  $!";

while(<WTAP_H>) {
    if ( /^#define WTAP_ENCAP_([A-Z0-9_]+)\s+(-?\d+)/ ) {
        $wtap_encaps_table .= "\t[\"$1\"] = $2,\n";
    }

    if ( /^#define WTAP_TSPREC_([A-Z0-9_]+)\s+(\d+)/ ) {
        $wtap_tsprecs_table .= "\t[\"$1\"] = $2,\n";
    }

    if ( /^#define WTAP_COMMENT_([A-Z0-9_]+)\s+(0x\d+)/ ) {
        $wtap_commenttypes_table .= "\t[\"$1\"] = $2,\n";
    }

    if ( /^#define REC_TYPE_([A-Z0-9_]+)\s+(\d+)\s+\/\*\*<([^\*]+)\*\// ) {
        $wtap_rec_types_table .= "\t[\"$1\"] = $2,  --$3\n";
    }

    if ( /^#define WTAP_HAS_([A-Z0-9_]+)\s+(0x\d+)\s+\/\*\*<([^\*]+)\*\// ) {
        my $num = hex($2);
        $wtap_presence_flags_table .= "\t[\"$1\"] = $num,  --$3\n";
    }
}

$wtap_encaps_table =~ s/,\n$/\n}\nwtap = wtap_encaps -- for bw compatibility\n/msi;
$wtap_tsprecs_table =~ s/,\n$/\n}\n/msi;
$wtap_commenttypes_table =~ s/,\n$/\n}\n/msi;
# wtap_rec_types_table has comments at the end (not a comma),
# but Lua doesn't care about extra commas so leave it in
$wtap_rec_types_table =~ s/\n$/\n}\n/msi;
# wtap_presence_flags_table has comments at the end (not a comma),
# but Lua doesn't care about extra commas so leave it in
$wtap_presence_flags_table =~ s/\n$/\n}\n/msi;

#
# Extract values from epan/ftypes/ftypes.h:
#
#	values from enum fttype
#

$ft_types_table = "-- Field Types\nftypes = {\n";
$frametypes_table = "-- Field Type FRAMENUM Types\nframetype = {\n";

my $ftype_num = 0;
my $frametypes_num = 0;

open FTYPES_H, "< $WSROOT/epan/ftypes/ftypes.h" or die "cannot open '$WSROOT/epan/ftypes/ftypes.h':  $!";
while(<FTYPES_H>) {
    if ( /^\s+FT_FRAMENUM_([A-Z0-9a-z_]+)\s*,/ ) {
        $frametypes_table .= "\t[\"$1\"] = $frametypes_num,\n";
        $frametypes_num++;
    } elsif ( /^\s+FT_([A-Z0-9a-z_]+)\s*,/ ) {
        $ft_types_table .= "\t[\"$1\"] = $ftype_num,\n";
        $ftype_num++;
    }
}
close FTYPES_H;

$ft_types_table =~ s/,\n$/\n}\n/msi;
$frametypes_table =~ s/,\n$/\n}\n/msi;

#
# Extract values from epan/proto.h:
#
#	values from enum base
#	#defines for encodings and expert group and severity levels
#

$bases_table        = "-- Display Bases\nbase = {\n";
$encodings          = "-- Encodings\n";
$expert_pi          = "-- Expert flags and facilities (deprecated - see 'expert' table below)\n";
$expert_pi_tbl      = "-- Expert flags and facilities\nexpert = {\n";
$expert_pi_severity = "\t-- Expert severity levels\n\tseverity = {\n";
$expert_pi_group    = "\t-- Expert event groups\n\tgroup = {\n";

open PROTO_H, "< $WSROOT/epan/proto.h" or die "cannot open '$WSROOT/epan/proto.h':  $!";

my $in_severity = 0;
my $prev_comment;
my $skip_this = 0;

while(<PROTO_H>) {
    $skip_this = 0;

    if (/^\s+(?:BASE|SEP)_([A-Z_]+)[ ]*=[ ]*([0-9]+)[,\s]+(?:\/\*\*< (.*?) \*\/)?/) {
        $bases_table .= "\t[\"$1\"] = $2,  -- $3\n";
    }

    if (/^#define\s+BASE_(RANGE_STRING)[ ]*((?:0x)?[0-9]+)[ ]+(?:\/\*\*< (.*?) \*\/)?/) {
        # Handle BASE_RANGE_STRING
        my $num = hex($2);
        $bases_table .= "\t[\"$1\"] = $num,  -- $3\n";
    }

    if (/^#define\s+BASE_(UNIT_STRING)[ ]*((?:0x)?[0-9]+)[ ]+(?:\/\*\*< (.*?) \*\/)?/) {
        # Handle BASE_UNIT_STRING as a valid base value in Lua
        my $num = hex($2);
        $bases_table .= "\t[\"$1\"] = $num,  -- $3\n";
    }

    if (/^.define\s+PI_SEVERITY_MASK /) {
        $in_severity = 1;
        $skip_this = 1;
    }

    if (/^.define\s+PI_GROUP_MASK /) {
        $in_severity = 2;
        $skip_this = 1;
    }

    if ($in_severity && /\/\*\* (.*?) \*\//) {
        $prev_comment = $1;
    }

    if ( /^.define\s+(PI_([A-Z_]+))\s+((0x)?[0-9A-Fa-f]+)/ ) {
        my ($name, $abbr, $value) = ($1, $2, hex($3));
        # I'm keeping this here for backwards-compatibility
        $expert_pi .= "$name = $value\n";

        if (!$skip_this && $in_severity == 1) {
            $expert_pi_severity .= "\t\t-- $prev_comment\n";
            $expert_pi_severity .= "\t\t\[\"$abbr\"\] = $value,\n";
        }
        elsif (!$skip_this && $in_severity == 2) {
            $expert_pi_group .= "\t\t-- $prev_comment\n";
            $expert_pi_group .= "\t\t\[\"$abbr\"\] = $value,\n";
        }
    }

    if ( /^.define\s+(ENC_[A-Z0-9_]+)\s+((0x)?[0-9A-Fa-f]+)/ ) {
        my ($name, $value) = ($1, hex($2));
        $encodings .= "$name = $value\n";
    }
}
close PROTO_H;

#
# Extract values from time_fmt.h:
#
#	ABSOLUTE_TIME_XXX values for absolute time bases
#

my $absolute_time_num = 0;

open TIME_FMT_H, "< $WSROOT/epan/time_fmt.h" or die "cannot open '$WSROOT/epan/time_fmt.h':  $!";
while(<TIME_FMT_H>) {
    if (/^\s+ABSOLUTE_TIME_([A-Z_]+)[ ]*=[ ]*([0-9]+)[,\s]+(?:\/\* (.*?) \*\/)?/) {
        $bases_table .= "\t[\"$1\"] = $2,  -- $3\n";
        $absolute_time_num = $2 + 1;
    } elsif (/^\s+ABSOLUTE_TIME_([A-Z_]+)[,\s]+(?:\/\* (.*?) \*\/)?/) {
        $bases_table .= "\t[\"$1\"] = $absolute_time_num,  -- $2\n";
        $absolute_time_num++;
    }
}
close TIME_FTM_H;

#
# Extract values from stat_groups.h:
#
#	MENU_X_X values for register_stat_group_t
#

$menu_groups .= "-- menu groups for register_menu\n";
my $menu_i = 0;

open STAT_GROUPS, "< $WSROOT/epan/stat_groups.h" or die "cannot open '$WSROOT/epan/stat_groups.h':  $!";
my $foundit = 0;
while(<STAT_GROUPS>) {
    # need to skip matching words in comments, and get to the enum
    if (/^typedef enum \{/) { $foundit = 1; }
    # the problem here is we need to pick carefully, so we don't break existing scripts
    if ($foundit && /REGISTER_([A-Z]+)_GROUP_(CONVERSATION|RESPONSE|ENDPOINT|[A-Z0-9_]+)/) {
        $menu_groups .= "MENU_$1_$2 = $menu_i\n";
        $menu_groups =~ s/_NONE//;
        $menu_i++;
    }
}
close STAT_GROUPS;


$bases_table .= "}\n";
$encodings .= "\n";
$expert_pi .= "\n";
$expert_pi_severity .= "\t},\n";
$expert_pi_group .= "\t},\n";
$expert_pi_tbl .= $expert_pi_group . $expert_pi_severity . "}\n\n";

for my $key (keys %replacements) {
    $template =~ s/%$key%/${$replacements{$key}}/msig;
}


print $template;
