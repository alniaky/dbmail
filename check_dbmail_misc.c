/*
 *  Copyright (C) 2006  Aaron Stone  <aaron@serendipity.cx>
 *  Copyright (c) 2005-2006 NFG Net Facilities Group BV support@nfg.nl
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   as published by the Free Software Foundation; either
 *   version 2 of the License, or (at your option) any later
 *   version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 *  $Id: check_dbmail_common.c 1598 2005-02-23 08:41:02Z paul $ 
 *
 *
 *  
 *
 *   Basic unit-test framework for dbmail (www.dbmail.org)
 *
 *   See http://check.sf.net for details and docs.
 *
 *
 *   Run 'make check' to see some action.
 *
 */ 

#include <check.h>
#include "check_dbmail.h"

extern char *configFile;
extern db_param_t _db_params;
extern int quiet;
extern int reallyquiet;

/*
 *
 * the test fixtures
 *
 */
void setup(void)
{
	reallyquiet = 1;
	configure_debug(5,0);
	config_read(configFile);
	GetDBParams(&_db_params);
	db_connect();
	auth_connect();
}

void teardown(void)
{
	db_disconnect();
	auth_disconnect();
}


START_TEST(test_g_strcasestr)
{
	char *s = "asdfqwer";
	fail_unless(g_strcasestr(s,"SD")!=NULL,"g_strcasestr failed 1");
	fail_unless(g_strcasestr(s,"er")!=NULL,"g_strcasestr failed 2");
	fail_unless(g_strcasestr(s,"As")!=NULL,"g_strcasestr failed 3");
}
END_TEST

Suite *dbmail_misc_suite(void)
{
	Suite *s = suite_create("Dbmail Misc");
	TCase *tc_misc = tcase_create("Misc");
	
	suite_add_tcase(s, tc_misc);
	
	tcase_add_checked_fixture(tc_misc, setup, teardown);
	tcase_add_test(tc_misc, test_g_strcasestr);
	
	return s;
}

int main(void)
{
	int nf;
	Suite *s = dbmail_misc_suite();
	SRunner *sr = srunner_create(s);
	srunner_run_all(sr, CK_NORMAL);
	nf = srunner_ntests_failed(sr);
	srunner_free(sr);
	return (nf == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
	
