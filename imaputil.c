/* 
 * imaputil.c
 *
 * IMAP-server utility functions implementations
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include "imaputil.h"
#include "imap4.h"
#include "debug.h"
#include "sstack.h"
#include "dbmysql.h"

#ifndef MAX_LINESIZE
#define MAX_LINESIZE 1024
#endif

#define BUFLEN 2048

extern const char AcceptedChars[];
extern const char AcceptedTagChars[];
extern const char AcceptedMailboxnameChars[];

char base64encodestring[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

const char *item_desc[] = 
{
  "TEXT", "HEADER", "MIME", "HEADER.FIELDS", "HEADER.FIELDS.NOT"
};

const char *envelope_items[] = 
{
  "from", "sender", "reply-to", "to", "cc", "bcc", "in-reply-to", NULL
};

/* 
 * retrieve_structure()
 *
 * retrieves the MIME-IMB structure of a message. The msg should be in the format
 * as build by db_fetch_headers().
 *
 * shows extension data if show_extension_data != 0
 *
 * returns -1 on error, 0 on success
 */
int retrieve_structure(FILE *outstream, mime_message_t *msg, int show_extension_data)
{
  struct mime_record *mr;
  struct element *curr;
  struct list *header_to_use;
  mime_message_t rfcmsg;
  char *subtype,*extension,*newline;

  fprintf(outstream,"(");

  mime_findfield("content-type", &msg->rfcheader, &mr);

  if (msg->mimeheader.start != NULL || !mr ||
      (mr && strncasecmp(mr->value,"multipart", strlen("multipart")) != 0))
    {
      /* show basic fields:
       * content-type, content-subtype, (parameter list), 
       * content-id, content-description, content-transfer-encoding,
       * size
       */
      
      if (msg->mimeheader.start == NULL)
	header_to_use = &msg->rfcheader;   /* we're dealing with a single-part RFC msg here */
      else
	header_to_use = &msg->mimeheader;  /* we're dealing with a pure-MIME header here */

      mime_findfield("content-type", header_to_use, &mr);
      if (mr && strlen(mr->value) > 0)
	show_mime_parameter_list(outstream, mr, 1, 0);
      else
	fprintf(outstream,"\"TEXT\" \"PLAIN\" NIL"); /* default */

      mime_findfield("content-id", header_to_use, &mr);
      if (mr && strlen(mr->value) > 0)
	fprintf(outstream, " \"%s\"",mr->value);
      else
	fprintf(outstream, " NIL");

      mime_findfield("content-description", header_to_use, &mr);
      if (mr && strlen(mr->value) > 0)
	fprintf(outstream, " \"%s\"",mr->value);
      else
	fprintf(outstream, " NIL");

      mime_findfield("content-transfer-encoding", header_to_use, &mr);
      if (mr && strlen(mr->value) > 0)
	fprintf(outstream, " \"%s\"",mr->value);
      else
	fprintf(outstream, " NIL");

      /* now output size */
      fprintf(outstream, " %lu ", msg->bodysize);


      /* now check special cases, first case: message/rfc822 */
      mime_findfield("content-type", header_to_use, &mr);
      if (mr && strncasecmp(mr->value, "message/rfc822", strlen("message/rfc822")) == 0 &&
	  header_to_use != &msg->rfcheader)
	{
	  /* msg/rfc822 found; extra items to be displayed:
	   * (a) body envelope of rfc822 msg
	   * (b) body structure of rfc822 msg
	   * (c) msg size (lines)
	   */
	  
	  if (retrieve_envelope(outstream, &msg->rfcheader) == -1)
	    return -1;

	  memmove(&rfcmsg, msg, sizeof(rfcmsg));
	  rfcmsg.mimeheader.start = NULL; /* forget MIME-part */
	  
	  if (retrieve_structure(outstream, &rfcmsg, show_extension_data) == -1)
	    return -1;
	  
	  /* output # of lines */
	  fprintf(outstream, " %lu", msg->bodylines);
	}

      /* second case: text 
       * NOTE: if 'content-type' is absent, TEXT is assumed 
       */
      if ((mr && strncasecmp(mr->value, "text", strlen("text")) == 0) || !mr)
	fprintf(outstream, "%lu", msg->bodylines);      /* output # of lines */

      if (show_extension_data)
	{
	  mime_findfield("content-md5", header_to_use, &mr);
	  if (mr && strlen(mr->value) > 0)
	    fprintf(outstream, " \"%s\"",mr->value);
	  else
	    fprintf(outstream, " NIL");

	  mime_findfield("content-disposition", header_to_use, &mr);
	  if (mr && strlen(mr->value) > 0)
	    {
	      fprintf(outstream, " (");
	      show_mime_parameter_list(outstream, mr, 0, 0);
	      fprintf(outstream, ")");
	    }
	  else
	    fprintf(outstream, " NIL");

	  mime_findfield("content-language", header_to_use, &mr);
	  if (mr && strlen(mr->value) > 0)
	    fprintf(outstream, " \"%s\"",mr->value);
	  else
	    fprintf(outstream, " NIL");
	}
    }
  else
    {
      /* check for a multipart message */
      mime_findfield("content-type", &msg->rfcheader, &mr);
      if (mr && strncasecmp(mr->value,"multipart", strlen("multipart")) == 0)
	{
	  curr = list_getstart(&msg->children);
	  while (curr)
	    {
	      if (retrieve_structure(outstream, (mime_message_t*)curr->data, 
				     show_extension_data) == -1)
		return -1;

	      curr = curr->nextnode;
	    }

	  /* show mulipart subtype */
	  subtype = strchr(mr->value, '/');
	  extension = strchr(subtype, ';');
	  
	  if (!subtype)
	    fprintf(outstream, " NIL");
	  else
	    {
	      if (!extension)
		{
		  newline = strchr(subtype, '\n');
		  if (!newline)
		    return -1;

		  *newline = 0;
		  fprintf(outstream, " \"%s\"", subtype+1);
		  *newline = '\n';
		}
	      else
		{
		  *extension = 0;
		  fprintf(outstream, " \"%s\"", subtype+1);
		  *extension = ';';
		}
	    }

	  /* show extension data (after subtype) */
	  if (extension && show_extension_data)
	    {
	      show_mime_parameter_list(outstream, mr, 0, 1);

	      /* FIXME: should give body-disposition & body-language here */
	      fprintf(outstream, " NIL NIL");
	    }
	}
      else
	{
	  /* ??? */
	}
    }
  fprintf(outstream,")");

  return 0;
}


/*
 * retrieve_envelope()
 *
 * retrieves the body envelope of an RFC-822 msg
 *
 * returns -1 on error, 0 on success
 */
int retrieve_envelope(FILE *outstream, struct list *rfcheader)
{
  struct mime_record *mr;
  int i,idx,delimiter,start;

  fprintf(outstream,"(");

  mime_findfield("date", rfcheader, &mr);
  if (mr && strlen(mr->value) > 0)
    fprintf(outstream, "\"%s\"",mr->value);
  else
    fprintf(outstream, "NIL");

  mime_findfield("subject", rfcheader, &mr);
  if (mr && strlen(mr->value) > 0)
    fprintf(outstream, " \"%s\"",mr->value);
  else
    fprintf(outstream, " NIL");

  /* now from, sender, reply-to, to, cc, bcc, in-reply-to fields;
   * note that multiple mailaddresses are separated by ','
   */

  for (idx=0; envelope_items[idx]; idx++)
    {
      mime_findfield(envelope_items[idx], rfcheader, &mr);
      if (mr && strlen(mr->value) > 0)
	{
	  fprintf(outstream," (");
      
	  /* find ',' to split up multiple addresses */
	  delimiter = 0;
	  
	  do
	    {
	      fprintf(outstream,"(");

	      start = delimiter;

	      for ( ; mr->value[delimiter] && mr->value[delimiter] != ','; delimiter++) ;

	      if (mr->value[delimiter])
		mr->value[delimiter] = 0; /* replace ',' by NULL-termination */
	      else
		delimiter = -1; /* this will be the last one */

	      /* the address currently being processed is now contained within
	       * &mr->value[start] 'till first '\0'
	       */
	      
	      /* two possibilities for the mail address:
	       * (1) name <user@domain>
	       * (2) user@domain
	       * scan for '<' to determine which case we should be dealing with 
	       */

	      for (i=start; mr->value[i] && mr->value[i] != '<'; i++) ;

	      if (mr->value[i] && i>start+2)
		{
		  /* name is contained in &mr->value[start] untill &mr->value[i-2] */
		  fprintf(outstream, "\"%.*s\"", i-start-1,&mr->value[start]);
		  start = i+1; /* skip to after '<' */
		}
	      else
		fprintf(outstream, "NIL");

	      fprintf(outstream, " NIL "); /* source route ?? smtp at-domain-list ?? */

	      /* now display user domainname; &mr->value[start] is starting point */
	      fprintf(outstream, "\"");

	      for (i=start; mr->value[i] && mr->value[i] != '>'; i++)
		{
		  if (mr->value[i] == '@')
		    fprintf(outstream,"\" \"");
		  else
		    fprintf(outstream,"%c",mr->value[i]);
		}

	      fprintf(outstream, "\"");
		  
	      if (delimiter > 0)
		mr->value[delimiter++] = ','; /* restore & prepare for next iteration */

	      fprintf(outstream, ")");

	    } while (delimiter > 0) ;
	  
	  fprintf(outstream,")");
	}
      else
	fprintf(outstream, " NIL");
  
    }

  mime_findfield("message-id", rfcheader, &mr);
  if (mr && strlen(mr->value) > 0)
    fprintf(outstream, " \"%s\"",mr->value);
  else
    fprintf(outstream, " NIL");

  fprintf(outstream,")");

  return 0;
}


/*
 * show_mime_parameter_list()
 *
 * shows mime name/value pairs, output to outstream
 * 
 * if force_subtype != 0 'NIL' will be outputted if no subtype is specified
 * if only_extension != 0 only extension data (after first ';') will be shown
 */
int show_mime_parameter_list(FILE *outstream, struct mime_record *mr, 
			     int force_subtype, int only_extension)
{
  int idx,delimiter,start,end;

  /* find first delimiter */
  for (delimiter = 0; mr->value[delimiter] && mr->value[delimiter] != ';'; delimiter++) ;

  if (mr->value[delimiter])
    mr->value[delimiter] = 0;
  else
    delimiter = -1;

  if (!only_extension)
    {
      /* find main type in value */
      for (idx = 0; mr->value[idx] && mr->value[idx] != '/'; idx++) ;
	  
      if (mr->value[idx] && (idx<delimiter || delimiter == -1))
	{
	  mr->value[idx] = 0;
	  fprintf(outstream,"\"%s\" \"%s\"", mr->value, &mr->value[idx+1]);
	  mr->value[idx] = '/';
	}
      else
	fprintf(outstream,"\"%s\" %s", mr->value, force_subtype ? "NIL" : "");
    }

  if (delimiter >= 0)
    {
      /* extra parameters specified */
      mr->value[delimiter] = ';';
      idx=delimiter;

      fprintf(outstream," (");

      /* extra params: <name>=<val> [; <name>=<val> [; ...etc...]]
	       * note that both name and val may or may not be enclosed by 
	       * either single or double quotation marks
	       */

      do
	{
	  /* skip whitespace */
	  for (idx++; isspace(mr->value[idx]); idx++) ;
		  
	  if (!mr->value[idx]) break; /* ?? */
		  
	  /* check if quotation marks are specified */
	  if (mr->value[idx] == '\"' || mr->value[idx] == '\'')
	    {
	      start = ++idx;
	      while (mr->value[idx] && mr->value[idx] != mr->value[start-1]) idx++;
		      
	      if (!mr->value[idx] || mr->value[idx+1] != '=') /* ?? no end quote */
		break;

	      end = idx;
	      idx+=2;        /* skip to after '=' */
	    }
	  else
	    {
	      start = idx;
	      while (mr->value[idx] && mr->value[idx] != '=') idx++;
		      
	      if (!mr->value[idx]) /* ?? no value specified */
		break;
		      
	      end = idx;
	      idx++;        /* skip to after '=' */
	    }

	  fprintf(outstream,"\"%.*s\" ", (end-start), &mr->value[start]);


	  /* now process the value; practically same procedure */

	  if (mr->value[idx] == '\"' || mr->value[idx] == '\'')
	    {
	      start = ++idx;
	      while (mr->value[idx] && mr->value[idx] != mr->value[start-1]) idx++;
		      
	      if (!mr->value[idx]) /* ?? no end quote */
		break;

	      end = idx;
	      idx++;
	    }
	  else
	    {
	      start = idx;

	      while (mr->value[idx] && !isspace(mr->value[idx]) &&
		     mr->value[idx] != ';') idx++;
		      
	      end = idx;
	    }

	  fprintf(outstream,"\"%.*s\"", (end-start), &mr->value[start]);
		  
		  /* check for more name/val pairs */
	  while (mr->value[idx] && mr->value[idx] != ';') idx++;

	  if (mr->value[idx])
	    fprintf(outstream," ");

	} while (mr->value[idx]); 

      fprintf(outstream,")");
	      
    }
  else
    {
      fprintf(outstream," NIL");
    }

  return 0;
}


/* 
 * get_part_by_num()
 *
 * retrieves a msg part by it's numeric specifier
 * 'part' is assumed to be valid! (i.e '1.2.3.44')
 * returns NULL if there is no such part 
 */
mime_message_t* get_part_by_num(mime_message_t *msg, const char *part)
{
  int nextpart,j;
  char *endptr;
  struct element *curr;

  if (part == NULL || strlen(part) == 0 || msg == NULL)
    return msg;


  nextpart = strtoul(part, &endptr, 10); /* strtoul() stops at '.' */

  for (j=1, curr=list_getstart(&msg->children); j<nextpart && curr; j++, curr = curr->nextnode);

  if (!curr)
    return NULL;

  if (*endptr)
    return get_part_by_num((mime_message_t*)curr->data, &endptr[1]); /* skip dot in part */

  return (mime_message_t*)curr->data;
}  


/*
 * rfcheader_dump()
 * 
 * dumps rfc-header fields belonging to rfcheader
 * the fields to be dumped are specified in fieldnames, an array containing nfields items
 *
 * the output will start after offset bytes and will have a maximum length of cnt bytes.
 *
 * if equal_type == 0 the field match criterium is inverted and non-matching fieldnames
 * will be selected
 */
int rfcheader_dump(FILE *outstream, struct list *rfcheader, char **fieldnames, int nfields,
		   int offset, int cnt, int equal_type, int showsize)
{
  struct mime_record *mr;
  struct element *curr;
  FILE *tmpfile;
  char tmpname[] = "rfcheader_out.tmp.XXXXXX";
  unsigned long size;

  curr = list_getstart(rfcheader);
  if (rfcheader == NULL || curr == NULL)
    {
      fprintf(outstream, "NIL\n");
      return 0;
    }

  tmpfile = fdopen(mkstemp(tmpname),"r+w");
  if (tmpfile == NULL)
    return -1; /* failed opening temporary file */
      
  curr = list_getstart(rfcheader);
  while (curr)
    {
      mr = (struct mime_record*)curr->data;

      if (haystack_find(nfields, fieldnames, mr->field) == equal_type)
	fprintf(tmpfile, "%s: %s\n", mr->field, mr->value);  /* ok output this field */

      curr = curr->nextnode;
    }
  fprintf(tmpfile,"\n");
  size = ftell(tmpfile);
  
  /* change var's if necessary */
  if (offset >= size)
    {
      offset = size;
      cnt = 0;
    }
  else
    {
      if (offset>=0 && cnt>=0)
	{
	  if (offset+cnt > size)
	    cnt = size-offset;
	}
      else
	{
	  offset = 0;
	  cnt = size;
	}
    }

  if (showsize)
    fprintf(outstream, "<%d> {%d}\n", offset, cnt);

  fseek(tmpfile, offset, SEEK_SET);

  /* output data */
  while (cnt-- > 0)
    fputc(fgetc(tmpfile),outstream);
	  
  fclose(tmpfile);
  unlink(tmpname);
  return 0;
}      
  

/*
 * mimeheader_dump()
 * 
 * dumps mime-header fields belonging to mimeheader
 *
 * the output will start after offset bytes and will have a maximum length of cnt bytes.
 */
int mimeheader_dump(FILE *outstream, struct list *mimeheader, int offset, int cnt)
{
  struct mime_record *mr;
  struct element *curr;
  FILE *tmpfile;
  char tmpname[] = "mimeheader_out.tmp.XXXXXX";
  unsigned long size;

  curr = list_getstart(mimeheader);
  if (mimeheader == NULL || curr == NULL)
    {
      fprintf(outstream, "NIL\n");
      return 0;
    }

  tmpfile = fdopen(mkstemp(tmpname),"r+w");
  if (tmpfile == NULL)
    return -1; /* failed opening temporary file */
      
  while (curr)
    {
      mr = (struct mime_record*)curr->data;
      fprintf(tmpfile, "%s: %s\n", mr->field, mr->value);
      curr = curr->nextnode;
    }
  fprintf(tmpfile,"\n");
  size = ftell(tmpfile);
  
  /* change var's if necessary */
  if (offset >= size)
    {
      offset = size;
      cnt = 0;
    }
  else
    {
      if (offset>=0 && cnt>=0)
	{
	  if (offset+cnt > size)
	    cnt = size-offset;
	}
      else
	{
	  offset = 0;
	  cnt = size;
	}
    }

  fprintf(outstream, "<%d> {%d}\n", offset, cnt);
  fseek(tmpfile, offset, SEEK_SET);

  /* output data */
  while (cnt-- > 0)
    fputc(fgetc(tmpfile),outstream);
	  
  fclose(tmpfile);
  unlink(tmpname);
  return 0;
}      


/* 
 * find a string in an array of strings
 */
int haystack_find(int haystacklen, char **haystack, const char *needle)
{
  int i;

  for (i=0; i<haystacklen; i++)
    if (strcasecmp(haystack[i], needle) == 0)
      return 1;

  return 0;
}


/*
 * get_fetch_items()
 *
 * retrieves the fetch item list (imap FETCH command) from an argument list.
 * the argument list is supposed to be in formatted according to 
 * build_args_array()
 *
 * returns -1 on error, 0 on succes, 1 on fault (wrong arg list)
 */
int get_fetch_items(char **args, fetch_items_t *fi)
{
  int i,j,inbody,bodyidx,invalidargs,shouldclose,delimpos,indigit,ispeek=0;

  /* init fetch item list */
  memset(fi, 0, sizeof(fetch_items_t));
  
  /* check multiple args: should be parenthesed */
  if (args[1] && strcmp(args[0],"(") != 0)
    {
      /* now args[0] should be 'body' or 'body.peek' */
      if (strcasecmp(args[0],"body") != 0 && strcasecmp(args[0],"body.peek") != 0)
	return 1;
      else
	{
	  /* next ']' should be last arg */
	  for (i=1; args[i] && strcmp(args[i],"]") != 0; i++) ;

	  if (!args[i]) /* impossible according to build-args-array */
	    return -1;

	  if (args[i+1])
	    return 1; /* wrong argument list */
	}	    
    }

  /* determine how many body-fields are needed */
  fi->nbodyfetches = 0;
  for (i=0; args[i]; i++)
    {
      if ((strcasecmp(args[i], "body") == 0 || strcasecmp(args[i], "body.peek") == 0) &&
	  args[i+1] && strcmp(args[i+1],"[") == 0)
	{
	  if (!args[i+2])
	    return 1; 

	  if (strcmp(args[i+2],"]") != 0)
	    fi->nbodyfetches++;

	  /* now walk on 'till this body[] finishes */
	  i+=2;
	  while (args[i] && strcmp(args[i],"]") != 0) i++;
	}
    }	

  trace(TRACE_DEBUG,"Found %d body items\n",fi->nbodyfetches);

  /* alloc mem */
  if (fi->nbodyfetches > 0)
    {
      fi->bodyfetches = (body_fetch_t*)malloc(sizeof(body_fetch_t) * fi->nbodyfetches);
      if (!fi->bodyfetches)
	return -1;  /* out of mem */

      memset(fi->bodyfetches, 0, sizeof(body_fetch_t) * fi->nbodyfetches);
    }
  else
    fi->bodyfetches = NULL;

  invalidargs = 0;
  inbody = 0;
  bodyidx = 0;

  i = 0;
  if (strcmp(args[i],"(") == 0) i++; /* skip parentheses */

  for ( ; args[i]; i++)
    {
      trace(TRACE_DEBUG,"a[i]: %s\n",args[i]);
      if (strcasecmp(args[i], "flags") == 0)
	{
	  fi->getFlags = 1;
	}
      else if (strcasecmp(args[i], "internaldate") == 0)
	{
	  fi->getInternalDate = 1;
	}
      else if (strcasecmp(args[i], "uid") == 0)
	{
	  fi->getUID = 1;
	}
      else if (strcasecmp(args[i], "rfc822") == 0)
	{
	  fi->getRFC822 = 1;
	}
      else if (strcasecmp(args[i], "rfc822.header") == 0)
	{
	  fi->getRFC822Header = 1;
	}
      else if (strcasecmp(args[i], "rfc822.size") == 0)
	{
	  fi->getSize = 1;
	}
      else if (strcasecmp(args[i], "rfc822.text") == 0)
	{
	  fi->getRFC822Text = 1;
	}
      else if (strcasecmp(args[i], "body") == 0 || strcasecmp(args[i],"body.peek") == 0)
	{
	  if (!args[i+1] || strcmp(args[i+1],"[") != 0)
	    {
	      if (strcasecmp(args[i],"body.peek") == 0)
		{
		  invalidargs = 1;
		  break;
		}
	      else
		{
		  fi->getMIME_IMB_noextension = 1; /* just BODY specified */
		}
	    }
	  else
	    {
	      /* determine wheter or not to set the seen flag */
	      ispeek = (strcasecmp(args[i],"body.peek") == 0);
	      
	      /* now read the argument list to body */
	      i++; /* now pointing at '[' (not the last arg, parentheses are matched) */
	      i++; /* now pointing at what should be the item type */

	      if (strcmp(args[i], "]") == 0)
		{
		  /* specified body[] or body.peek[] */
		  if (ispeek)
		    fi->getBodyTotalPeek = 1;
		  else
		    fi->getBodyTotal = 1;

		  /* do not increment i, it is done by the for-loop */
		  continue;
		}
	      
	      if (ispeek)
		fi->bodyfetches[bodyidx].noseen = 1;

	      /* first check if there is a partspecifier (numbers & dots) */
	      indigit = 0;
	      for (j=0; args[i][j]; j++)
		{
		  if (isdigit(args[i][j]))
		    {
		      indigit = 1;
		      continue;
		    }
		  else if (args[i][j] == '.')
		    {
		      if (!indigit)
			{
			  /* error, single dot specified */
			  invalidargs = 1;
			  break;
			}
		      
		      indigit = 0;
		      continue;
		    }
		  else
		    break; /* other char found */
		}
	      
	      if (invalidargs)
		break;

	      if (j > 0)
		{
		  if (indigit)
		    {
		      /* wrong */
		      invalidargs = 1;
		      break;
		    }

		  /* partspecifier present, save it */
		  if (j >= IMAP_MAX_PARTSPEC_LEN)
		    {
		      invalidargs = 1;
		      break;
		    }
		  strncpy(fi->bodyfetches[bodyidx].partspec, args[i], j);
		}
	      fi->bodyfetches[bodyidx].partspec[j] = '\0';

	      shouldclose = 0;
	      if (strcasecmp(&args[i][j], "text") == 0)
		{
		  fi->bodyfetches[bodyidx].itemtype = BFIT_TEXT;
		  shouldclose = 1;
		}
	      else if (strcasecmp(&args[i][j], "header") == 0)
		{
		  fi->bodyfetches[bodyidx].itemtype = BFIT_HEADER;
		  shouldclose = 1;
		}
	      else if (strcasecmp(&args[i][j], "mime") == 0)
		{
		  if (j == 0)
		    {
		      /* no can do */
		      invalidargs = 1;
		      break;
		    }
		  fi->bodyfetches[bodyidx].itemtype = BFIT_MIME;
		  shouldclose = 1;
		}
	      else if (strcasecmp(&args[i][j], "header.fields") == 0)
		fi->bodyfetches[bodyidx].itemtype = BFIT_HEADER_FIELDS;
	      else if (strcasecmp(&args[i][j], "header.fields.not") == 0)
		fi->bodyfetches[bodyidx].itemtype = BFIT_HEADER_FIELDS_NOT;
	      else
		{
		  invalidargs = 1;
		  break;
		}

	      if (shouldclose)
		{
		  if (strcmp(args[i+1],"]") != 0)
		    {
		      invalidargs = 1;
		      break;
		    }
		}
	      else
		{
		  i++; /* should be at '(' now */
		  if (strcmp(args[i],"(") != 0)
		    {
		      invalidargs = 1;
		      break;
		    }
		  
		  i++; /* at first item of field list now, remember idx */
		  fi->bodyfetches[bodyidx].argstart = i;

		  /* walk on untill list terminates (and it does 'cause parentheses are matched) */
		  while (strcmp(args[i],")") != 0)
		    i++;

		  fi->bodyfetches[bodyidx].argcnt = i - fi->bodyfetches[bodyidx].argstart;
		  
		  if (fi->bodyfetches[bodyidx].argcnt == 0)
		    {
		      invalidargs = 1;
		      break;
		    }

		  /* next argument should be ']' */
		  if (strcmp(args[i+1],"]") != 0)
		    {
		      invalidargs = 1;
		      break;
		    }
		}
	      
	      i++; /* points to ']' now */

	      /* check if octet start/cnt is specified */
	      if (args[i+1] && args[i+1][0] == '<')
		{
		  i++; /* advance */

		  /* check argument */
		  if (args[i][strlen(args[i]) - 1 ] != '>')
		    {
		      invalidargs = 1;
		      break;
		    }

		  delimpos = -1;
		  for (j=1; j < strlen(args[i])-1; j++)
		    {
		      if (args[i][j] == '.')
			{
			  if (delimpos != -1)
			    {
			      invalidargs = 1;
			      break;
			    }
			  delimpos = j;
			}
		      else if (!isdigit(args[i][j]))
			{
			  invalidargs = 1;
			  break;
			}
		    }

		  if (invalidargs)
		    break;

		  if (delimpos == -1 || delimpos == 1 || delimpos == (strlen(args[i])-2) )
		    {
		      /* no delimiter found or at first/last pos */
		      invalidargs = 1;
		      break;
		    }

		  /* read the numbers */
		  args[i][strlen(args[i]) - 1] = '\0';
		  args[i][delimpos] = '\0';
		  fi->bodyfetches[bodyidx].octetstart = atoi(&args[i][1]);
		  fi->bodyfetches[bodyidx].octetcnt   = atoi(&args[i][delimpos+1]);
			
		  /* restore argument */
		  args[i][delimpos] = '.';
		  args[i][strlen(args[i]) - 1] = '>';
		}
	      else
		{
		  fi->bodyfetches[bodyidx].octetstart = 0;
		  fi->bodyfetches[bodyidx].octetcnt   = -1;
		}
	      /* ok all done for body item */
	      bodyidx++;
	    }
	}
      else if (strcasecmp(args[i], "all") == 0)
	{
	  fi->getFlags = 1;
	  fi->getInternalDate = 1;
	  fi->getSize = 1;
	  fi->getEnvelope = 1;
	}
      else if (strcasecmp(args[i], "fast") == 0)
	{
	  fi->getFlags = 1;
	  fi->getInternalDate = 1;
	  fi->getSize = 1;
	}
      else if (strcasecmp(args[i], "full") == 0)
	{
	  fi->getFlags = 1;
	  fi->getInternalDate = 1;
	  fi->getSize = 1;
	  fi->getEnvelope = 1;
	  fi->getMIME_IMB = 1;
	}
      else if (strcasecmp(args[i], "bodystructure") == 0)
	{
	  fi->getMIME_IMB = 1;
	}
      else if (strcasecmp(args[i], "envelope") == 0)
	{
	  fi->getEnvelope = 1;
	}
      else if (strcmp(args[i], ")") == 0)
	{
	  /* only allowed if last arg here */
	  if (args[i+1])
	    {
	      invalidargs = 1;
	      break;
	    }
	}
      else
	{
	  invalidargs = 1;
	  break;
	}
    }

  if (invalidargs)
    {
      trace(TRACE_DEBUG,"invalid argument detected at %d: '%s'\n",i,args[i]);

      free(fi->bodyfetches);
      fi->bodyfetches = NULL;
      return 1;
    }

  /* DEBUG dump read item list */
  trace(TRACE_DEBUG,"get_item_list():\n");
  trace(TRACE_DEBUG,"Got %d BODY / BODY.PEEK items\n",fi->nbodyfetches);
  trace(TRACE_DEBUG,"Comparing with bodyidx now: %d\n",bodyidx);
  
  for (i=0; i<fi->nbodyfetches; i++)
    {
      trace(TRACE_DEBUG,"bodyfetch, type %d ('%s')\n",fi->bodyfetches[i].itemtype,
	    item_desc[fi->bodyfetches[i].itemtype]);
      trace(TRACE_DEBUG,"noseen: %d\n",fi->bodyfetches[i].noseen);
      trace(TRACE_DEBUG,"size delimited: %d %d\n", fi->bodyfetches[i].octetstart,
	    fi->bodyfetches[i].octetcnt);

      trace(TRACE_DEBUG,"arguments: \n");

      for (j = fi->bodyfetches[i].argstart; 
	   j < fi->bodyfetches[i].argstart+fi->bodyfetches[i].argcnt;
	   j++)
	trace(TRACE_DEBUG,"  arg[%d]: '%s'\n",j-fi->bodyfetches[i].argstart,args[j]);
    }

  return 0;

}

/*
 * give_chunks()
 *
 * splits up a string delimited by a given character into a NULL-terminated array of strings
 * does not perform any quotation-mark checks
 */
char **give_chunks(const char *str, char delimiter)
{
  int cnt,i;
  char **array,*cpy,*tmp;
  
  cpy = (char*)malloc(sizeof(char) * (strlen(str) + 1));
  if (!cpy  )
    {
      trace(TRACE_ERROR, "give_chunks(): out of memory\n");
      return NULL;
    }

  strcpy(cpy,str);
  tmp = cpy;      /* save start of cpy */

  for (i=0,cnt=0; str[i]; i++)
    if (str[i] == delimiter) 
      {
	cnt++;
	cpy[i] = '\0';
      }

  cnt++; /* add last part */

  /* alloc mem */
  cnt++; /* for NULL termination */
  array = (char**)malloc(sizeof(char*) * cnt);

  if (!array)
    {
      trace(TRACE_ERROR, "give_chunks(): out of memory\n");
      free(cpy);
      return NULL;
    }

  for (i=0,cnt=0; str[i]; i++)
    {
      if (str[i] == delimiter)
	{
	  array[cnt++] = cpy;   /* save this address */
	  cpy = &tmp[i+1];      /* let cpy point to next string */
	}
    }

  /* copy last part */
  array[cnt++] = cpy;

  array[cnt] = NULL;
  return array;
}


/* 
 * free_chunks()
 *
 * frees memory allocated using give_chunks()
 */
void free_chunks(char **chunks)
{
  if (!chunks)
    return;

  if (chunks[0])
    free(chunks[0]); /* the entire array will be freed now */

  free(chunks); /* free ptrs to strings */
}



/*
 * check_state_and_args()
 *
 * checks if the user is in the right state & the numbers of arguments;
 * a state of -1 specifies any state
 * arguments can be grouped by means of parentheses
 *
 * returns 1 on succes, 0 on failure
 */
int check_state_and_args(const char *command, const char *tag, char **args, 
			 int nargs, int state, ClientInfo *ci)
{
  int i;
  imap_userdata_t *ud = (imap_userdata_t*)ci->userData;

  /* check state */
  if (state != -1)
    {
      if (ud->state != state)
	{
	  if (!(state == IMAPCS_AUTHENTICATED && ud->state == IMAPCS_SELECTED))
	    {
	      fprintf(ci->tx,"%s BAD %s command received in invalid state\n", tag, command);
	      return 0;
	    }
	}
    }

  /* check args */
  for (i=0; i<nargs; i++)
    {
      if (!args[i])
	{
	  /* error: need more args */
	  fprintf(ci->tx,"%s BAD missing argument%s to %s\n", tag, 
		  (nargs == 1) ? "" : "(s)", command);
	  return 0;
	}
    }

  for (i=0; args[i]; i++) ;

  if (i > nargs)
    {
      /* error: too many args */
      fprintf(ci->tx,"%s BAD too many arguments to %s\n",tag, command);
      return 0;
    }

  /* succes */
  return 1;
}



/*
 * build_args_array()
 *
 * builds an dimensional array of strings containing arguments based upon 
 * a series of arguments passed as a single string.
 * normal/square parentheses have special meaning:
 * '(body [all header])' will result in the following array:
 * [0] = '('
 * [1] = 'body'
 * [2] = '['
 * [3] = 'all'
 * [4] = 'header'
 * [5] = ']'
 * [6] = ')'
 *
 * quoted strings are those enclosed by double quotation marks and returned as a single argument
 * WITHOUT the enclosing quotation marks
 *
 * parentheses loose their special meaning if inside (double)quotation marks;
 * data should be 'clarified' (see clarify_data() function below)
 *
 * The returned array will be NULL-terminated.
 * Will return NULL upon errors.
 */

/* local defines */
#define NORMPAR 1 
#define SQUAREPAR 2
#define NOPAR 0

char **build_args_array(const char *s)
{
  char **args;
  char *scpy;
  int nargs=0,inquote=0,i,quotestart,currarg;
  int nnorm=0,nsquare=0,paridx=0;
  char parlist[MAX_LINESIZE];

  if (!s)
    return NULL;

  /* check for empty string */
  if (!(*s))
    {
      args = (char**)malloc(sizeof(char*));
      if (!args)
	{
	  trace(TRACE_MESSAGE, "IMAPD: Not enough memory while building up argument array.");
	  return NULL;
	}

      args[0] = NULL;
      return args;
    }

  scpy = (char*)malloc(sizeof(char)* (strlen(s)+1));
  if (!scpy)
    {
      trace(TRACE_MESSAGE, "IMAPD: Not enough memory while building up argument array.");
      return NULL;
    }

  /* copy original to scpy */
  strcpy(scpy,s);

  /* now replace all delimiters by \0 */
  for (i=0,inquote=0; i<strlen(s); i++)
    {
      if (scpy[i] == '"')
	{
	  if ((i>0 && scpy[i-1]!='\\') || i==0)
	    {
	      /* toggle in-quote flag */
	      inquote ^= 1;
	    }
	}

      if ((scpy[i] == ' ' || scpy[i] == '(' || scpy[i] == ')' ||
	   scpy[i] == '[' || scpy[i] == ']') && !inquote)
	{
	  scpy[i] = '\0';
	}
    }

  
  /* count the arguments */
  paridx = 0;
  parlist[paridx] = NOPAR;

  for (i=0,nargs=0; i<strlen(s); i++)
    {
      if (!scpy[i])
	{
	  /* check for ( or ) in original string */
	  if (s[i] == '(' || s[i] == ')' || s[i] == '[' || s[i] == ']')
	    nargs++;

	  /* check parenthese structure */
	  if (s[i] == ')')
	    {
	      if (paridx < 0 || parlist[paridx] != NORMPAR)
		{
		  free(scpy);
		  return NULL;
		}
	      else
		{
		  nnorm--;
		  paridx--;
		}
	    }

	  if (s[i] == ']')
	    {
	      if (paridx < 0 || parlist[paridx] != SQUAREPAR)
		{
		  free(scpy);
		  return NULL;
		}
	      else
		{
		  paridx--;
		  nsquare--;
		}
	    }

	  if (s[i] == '(')
	    {
	      parlist[++paridx] = NORMPAR;
	      nnorm++;
	    }

	  if (s[i] == '[')
	    {
	      parlist[++paridx] = SQUAREPAR;
	      nsquare++;
	    }

	  continue;
	}

      if (scpy[i] == '"')
	{
	  if ((i>0 && scpy[i-1]!='\\') || i==0)
	    {
	      /* toggle in-quote flag */
	      inquote ^= 1;
	      if (inquote)
		nargs++;
	    }
	}
      else
	{
	  if (!inquote)
	    {
	      /* at an argument now, proceed to end (before next NULL char) */
	      while (scpy[i] && i<strlen(s)) i++;
	      i--;
	      nargs++;
	    }
	}
    }

  if (paridx != 0)
    {
      free(scpy);
      return NULL;
    }

  /* alloc memory */
  args = (char**)malloc((nargs+1) * sizeof(char *));
  if (!args)
    {
      /* out of mem */
      trace(TRACE_MESSAGE, "IMAPD: Not enough memory while building up argument array.");
      free(scpy);
      return NULL;
    }

  /* single out the arguments */
  currarg = 0;
  for (i=0; i<strlen(s); i++)
    {
      if (!scpy[i])
	{
	  /* check for ( or ) in original string */
	  if (s[i] == '(' || s[i] == ')' || s[i] == '[' || s[i] ==']')
	    {
	      /* add parenthesis */
	      /* alloc mem */
	      args[currarg] = (char*)malloc(sizeof(char) * 2);

	      if (!args[currarg])
		{
		  /* out of mem */
		  /* free currently allocated mem */
		  trace(TRACE_MESSAGE, "IMAPD: Not enough memory while building up argument array.");

		  for (i=0; i<currarg; i++)
		    free(args[i]);
	      
		  free(args);
		  free(scpy);
		  return NULL;
		}

	      args[currarg][0] = s[i];
	      args[currarg][1] = '\0';
	      currarg++;
	    }
	  continue;
	}

      if (scpy[i] == '"')
	{
	  if ((i>0 && s[i-1]!='\\') || i==0)
	    {
	      /* toggle in-quote flag */
	      inquote ^= 1;
	      if (inquote)
		{
		  /* just started the quotation, remember idx */
		  quotestart = i;
		}
	      else
		{
		  /* alloc mem */
		  args[currarg] = (char*)malloc(sizeof(char) * (i-quotestart+1+1-2));
		  if (!args[currarg])
		    {
		      /* out of mem */
		      /* free currently allocated mem */
		      trace(TRACE_MESSAGE, "IMAPD: Not enough memory while building up argument array.");

		      for (i=0; i<currarg; i++)
			free(args[i]);

		      free(args);
		      free(scpy);
		      return NULL;
		    }

		  /* copy quoted string */
		  memcpy(args[currarg], &s[quotestart+1], sizeof(char)*(i-quotestart-1));
		  args[currarg][i-quotestart-1] = '\0'; 
		  currarg++;
		}
	    }
	}
      else if (!inquote)
	{
	  /* at an argument now, save & proceed to end (before next NULL char) */
	  /* alloc mem */
	  args[currarg] = (char*)malloc(sizeof(char) * (strlen(&scpy[i])+1) );
	  if (!args[currarg])
	    {
	      /* out of mem */
	      /* free currently allocated mem */
	      trace(TRACE_MESSAGE, "IMAPD: Not enough memory while building up argument array.");

	      for (i=0; i<currarg; i++)
		free(args[i]);
	      
	      free(args);
	      free(scpy);
	      return NULL;
	    }

	  /* copy arg */
	  memcpy(args[currarg], &scpy[i], sizeof(char)*(strlen(&scpy[i])+1) );
	  currarg++;

	  while (scpy[i] && i<strlen(s)) i++;
	  i--;
	}
    }
  if (inquote)
    {
      /* single quotation mark, treat as single argument */
      args[currarg] = (char*)malloc(sizeof(char) * (strlen(s)-quotestart));
      if (!args[currarg])
	{
	  /* out of mem */
	  /* free currently allocated mem */
	  trace(TRACE_MESSAGE, "IMAPD: Not enough memory while building up argument array.");

	  for (i=0; i<currarg; i++)
	    free(args[i]);

	  free(args);
	  return NULL;
	}

      /* copy quoted string */
      memcpy(args[currarg], &s[quotestart+1], sizeof(char)*(strlen(s)-quotestart-1));
      args[currarg][strlen(s)-quotestart-1] = '\0'; 
      currarg++;
    }

  args[currarg] = NULL; /* terminate array */

  free(scpy);

  /* dump args (debug) */
  for (i=0; args[i]; i++)
    {
      trace(TRACE_MESSAGE, "arg[%d]: '%s'\n",i,args[i]);
    }

  return args;
}
#undef NOPAR
#undef NORMPAR
#undef RIGHTPAR


/*
 * clarify_data()
 *
 * replaces all multiple spaces by a single one except for quoted spaces;
 * removes leading and trailing spaces and a single trailing newline (if present)
 */
void clarify_data(char *str)
{
  int startidx,i,inquote,endidx;


  /* remove leading spaces */
  for (i=0; str[i] == ' '; i++) ;
  memmove(str, &str[i], sizeof(char) * (strlen(&str[i])+1)); /* add one for \0 */

  /* remove CR/LF */
  endidx = strlen(str)-1;
  if (endidx >= 0 && (str[endidx] == '\n' ||str[endidx] == '\r'))
    endidx--;

  if (endidx >= 0 && (str[endidx] == '\n' ||str[endidx] == '\r'))
    endidx--;


  if (endidx == 0)
    {
      /* only 1 char left and it is not a space */
      str[1] = '\0';
      return;
    }

  /* remove trailing spaces */
  for (i=endidx; i>0 && str[i] == ' '; i--) ;
  if (i == 0)
    {
      /* empty string remains */
      *str = '\0';
      return;
    }

  str[i+1] = '\0';

  /* scan for multiple spaces */
  inquote = 0;
  for (i=0; i < strlen(str); i++)
    {
      if (str[i] == '"')
	{
	  if ((i > 0 && str[i-1]!='\\') || i == 0)
	    {
	      /* toggle in-quote flag */
	      inquote ^= 1;
	    }
	}

      if (str[i] == ' ' && !inquote)
	{
	  for (startidx = i; str[i] == ' '; i++);

	  if (i-startidx > 1)
	    {
	      /* multiple non-quoted spaces found --> remove 'm */
	      memmove(&str[startidx+1], &str[i], sizeof(char) * (strlen(&str[i])+1));
	      /* update i */
	      i = startidx+1;
	    }
	}
    }
}      
     

/*
 * retourneert de idx v/h eerste voorkomen van ch in s,
 * of strlen(s) als ch niet voorkomt
 */
int stridx(const char *s, char ch)
{
  int i;

  for (i=0; s[i] && s[i] != ch;  i++) ;

  return i;
}


/*
 * checkchars()
 *
 * performs a check to see if the read data is valid
 * returns 0 if invalid, 1 otherwise
 */
int checkchars(const char *s)
{
  int i;

  for (i=0; s[i]; i++)
    {
      if (stridx(AcceptedChars, s[i]) == strlen(AcceptedChars))
	{
	  /* wrong char found */
	  return 0;
	}
    }
  return 1;
}


/*
 * checktag()
 *
 * performs a check to see if the read data is valid
 * returns 0 if invalid, 1 otherwise
 */
int checktag(const char *s)
{
  int i;

  for (i=0; s[i]; i++)
    {
      if (stridx(AcceptedTagChars, s[i]) == strlen(AcceptedTagChars))
	{
	  /* wrong char found */
	  return 0;
	}
    }
  return 1;
}


/*
 * checkmailboxname()
 *
 * performs a check to see if the mailboxname is valid
 * returns 0 if invalid, 1 otherwise
 */
int checkmailboxname(const char *s)
{
  int i;

  if (strlen(s) == 0)
    return 0; /* empty name is not valid */

  if (strlen(s) >= IMAP_MAX_MAILBOX_NAMELEN)
    return 0; /* a too large string is not valid */

  /* check for invalid characters */
  for (i=0; s[i]; i++)
    {
      if (stridx(AcceptedMailboxnameChars, s[i]) == strlen(AcceptedMailboxnameChars))
	{
	  /* wrong char found */
	  return 0;
	}
    }

  /* check for double '/' */
  for (i=1; s[i]; i++)
    {
      if (s[i] == '/' && s[i-1] == '/')
	return 0;
    }

  /* check if the name consists of a single '/' */
  if (strlen(s) == 1 && s[0] == '/')
    return 0;

  return 1;
}

  

/*
 * base64encode()
 *
 * encodes a string using base64 encoding
 */
void base64encode(char *in,char *out)
{
  for ( ; strlen(in) >= 3; in+=3)
    {
      *out++ = base64encodestring[ (in[0] & 0xFC) >> 2U];
      *out++ = base64encodestring[ ((in[0] & 0x03) << 4U) | ((in[1] & 0xF0) >> 4U) ];
      *out++ = base64encodestring[ ((in[1] & 0x0F) << 2U) | ((in[2] & 0xC0) >> 6U) ];
      *out++ = base64encodestring[ (in[2] & 0x3F) ];
    }

  if (strlen(in) == 2)
    {
      /* 16 bits left to encode */
      *out++ = base64encodestring[ (in[0] & 0xFC) >> 2U];
      *out++ = base64encodestring[ ((in[0] & 0x03) << 4U) | ((in[1] & 0xF0) >> 4U) ];
      *out++ = base64encodestring[ ((in[1] & 0x0F) << 2U) ];
      *out++ = '=';

      return;
    }
      
  if (strlen(in) == 1)
    {
      /* 8 bits left to encode */
      *out++ = base64encodestring[ (in[0] & 0xFC) >> 2U];
      *out++ = base64encodestring[ ((in[0] & 0x03) << 4U) ];
      *out++ = '=';
      *out++ = '=';

      return;
    }
}      


/*
 * base64decode()
 *
 * decodes a base64 encoded string
 */
void base64decode(char *in,char *out)
{
  for ( ; strlen(in) >= 4; in+=4)
    {
      *out++ = (stridx(base64encodestring, in[0]) << 2U) 
	| ((stridx(base64encodestring, in[1]) & 0x30) >> 4U);

      *out++ = ((stridx(base64encodestring, in[1]) & 0x0F) << 4U) 
	| ((stridx(base64encodestring, in[2]) & 0x3C) >> 2U);

      *out++ = ((stridx(base64encodestring, in[2]) & 0x03) << 6U) 
	| (stridx(base64encodestring, in[3]) & 0x3F);
    }

  *out = 0;
}      


/*
 * binary_search()
 *
 * performs a binary search on array to find key
 * array should be ascending in values
 *
 * returns index of key in array or -1 if not found
 */
int binary_search(const unsigned long *array, int arraysize, unsigned long key)
{
  int low,high,mid;

  low = 0;
  high = arraysize-1;

  while (low <= high)
    {
      mid = (high+low)/2;
      if (array[mid] < key)
	low = mid+1;
      else if (array[mid] > key)
	high = mid-1;
      else
	return mid;
    }

  return -1; /* not found */
}

