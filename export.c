/*
 Copyright (C) 2005-2006 NFG Net Facilities Group BV, support@nfg.nl

 This program is free software; you can redistribute it and/or 
 modify it under the terms of the GNU General Public License 
 as published by the Free Software Foundation; either 
 version 2 of the License, or (at your option) any later 
 version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* $Id: user.c 1891 2005-10-03 10:01:21Z paul $
 * This is the dbmail-user program
 * It makes adding users easier 
 *
 *
 * - moving most code to dbmail-users.c. Just a thin wrapper left */

#include "dbmail.h"

char *configFile = DEFAULT_CONFIG_FILE;

#define PNAME "dbmail/export"

int quiet, reallyquiet, verbose;

extern db_param_t _db_params;

/* UI policy */

int do_showhelp(void) {
	
	printf("*** dbmail-export ***\n");
	printf("Use this program to export your DBMail mailboxes.\n");
	printf("     -u username   specify a user\n");
	printf("     -m mailbox    specify a mailbox\n");
	printf("     -o outfile    specify the destination mbox (default ./user/mailbox)\n");
	printf("\n");
	printf("Summary of options for all modes:\n");
	printf("\n");
        printf("Common options for all DBMail utilities:\n");
	printf("     -f file   specify an alternative config file\n");
	printf("     -q        quietly skip interactive prompts\n"
	       "               use twice to suppress error messages\n");
	printf("     -v        verbose details\n");
	printf("     -V        show the version\n");
	printf("     -h        show this help message\n");
	
	return 0;
	
}

int main(int argc, char *argv[])
{
	int opt = 0, opt_prev = 0;
	int show_help = 0;
	int result = 0;
	char *user = NULL,*mailbox=NULL, *outfile=NULL;
	FILE *ostream;
	struct DbmailMailbox *mb = NULL;
	u64_t useridnr = 0, mailbox_idnr = 0;

	openlog(PNAME, LOG_PID, LOG_MAIL);
	setvbuf(stdout, 0, _IONBF, 0);

	g_mime_init(0);

	/* get options */
	opterr = 0;		/* suppress error message from getopt() */
	while ((opt = getopt(argc, argv,
		"-u:m:o:" /* Major modes */
		"f:qvVh" /* Common options */ )) != -1) {
		/* The initial "-" of optstring allows unaccompanied
		 * options and reports them as the optarg to opt 1 (not '1') */
		if (opt == 1)
			opt = opt_prev;
		opt_prev = opt;

		switch (opt) {
		/* Major modes of operation
		 * (exactly one of these is required) */
		case 'u':
			if (optarg && strlen(optarg))
				user = optarg;
			break;

		case 'm':
			if (optarg && strlen(optarg))
				mailbox = optarg;
			break;
		case 'o':
			if (optarg && strlen(optarg))
				outfile = optarg;
			break;

		/* Common options */
		case 'f':
			if (optarg && strlen(optarg) > 0)
				configFile = optarg;
			else {
				qerrorf("dbmail-mailbox: -f requires a filename\n\n");
				result = 1;
			}
			break;

		case 'h':
			show_help = 1;
			break;

		case 'q':
			/* If we get q twice, be really quiet! */
			if (quiet)
				reallyquiet = 1;
			if (!verbose)
				quiet = 1;
			break;

		case 'v':
			if (!quiet)
				verbose = 1;
			break;

		case 'V':
			/* Show the version and return non-zero. */
			printf("\n*** DBMAIL: dbmail-users version "
			       "$Revision: 1891 $ %s\n\n", COPYRIGHT);
			result = 1;
			break;

		default:
			/* printf("unrecognized option [%c], continuing...\n",optopt); */
			break;
		}

		/* If there's a non-negative return code,
		 * it's time to free memory and bail out. */
		if (result)
			goto freeall;
	}	

	/* If nothing is happening, show the help text. */
	if (!user || !mailbox || show_help) {
		do_showhelp();
		result = 1;
		goto freeall;
	}

	if (! outfile) {
		GString *t = g_string_new("");
		g_string_printf(t, "%s/%s", user, mailbox);
		outfile=t->str;
		g_string_free(t,FALSE);
	}

	/* read the config file */
        if (config_read(configFile) == -1) {
                qerrorf("Failed. Unable to read config file %s\n", configFile);
                result = -1;
                goto freeall;
        }
                
	SetTraceLevel("DBMAIL");
	GetDBParams(&_db_params);

	/* open database connection */
	if (db_connect() != 0) {
		qerrorf ("Failed. Could not connect to database (check log)\n");
		result = -1;
		goto freeall;
	}

	/* open authentication connection */
	if (auth_connect() != 0) {
		qerrorf("Failed. Could not connect to authentication (check log)\n");
		result = -1;
		goto freeall;
	}

	/* Verify the existence of this user */
	if (auth_user_exists(user, &useridnr) == -1) {
		qerrorf("Error: cannot verify existence of user [%s].\n", user);
		result = -1;
		goto freeall;
	}
	if (useridnr == 0) {
		qerrorf("Error: user [%s] does not exist.\n", user);
		result = -1;
		goto freeall;
	}
	if (db_findmailbox(mailbox, useridnr, &mailbox_idnr) != 1) {
		qerrorf("Error: cannot verify existence of mailbox [%s].\n", mailbox);
		result = -1;
		goto freeall;
	}

	qerrorf("exporting [%s/%s]\n", user, mailbox);
	mb = dbmail_mailbox_new(mailbox_idnr);

	if (! (ostream = fopen(outfile,"a"))) {
		int err=errno;
		qerrorf("opening [%s] failed [%s]", outfile, strerror(err));
		result = -1;
		goto freeall;
	}
	
	if (dbmail_mailbox_dump(mb,ostream) < 0)
		qerrorf("exporing failed\n");
	else
		qerrorf("exporting finished\n");

	/* Here's where we free memory and quit.
	 * Be sure that all of these are NULL safe! */
freeall:
	
	if (mb)
		dbmail_mailbox_free(mb);
	
	db_disconnect();
	auth_disconnect();
	config_free();
	g_mime_shutdown();

	if (result < 0)
		qerrorf("Command failed.\n");
	return result;
}

