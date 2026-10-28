/**************************************************************************
*  File: improved-edit.c                                   Part of tbaMUD *
*  Usage: Routines specific to the improved editor.                       *
**************************************************************************/

/**
 * @file improved-edit.c
 * The "/" commands of the line editor: list, insert, delete, edit, format,
 * replace, save and abort.
 *
 * Who controls the input: the command line is typed by the player (one line,
 * at most MAX_INPUT_LENGTH, printable ASCII only). The buffer being edited,
 * *d->str, can also hold text loaded from world, help or lib files, so it
 * may lack a final newline and may contain colour codes, including a lone
 * trailing '\t'.
 *
 * Ownership: *d->str is malloc'd and owned by the editing session (see
 * string_write() in modify.c). Commands that change it reallocate it, so
 * nothing may keep a pointer into it across a command.
 */

#include "conf.h"
#include "sysdep.h"
#include "structs.h"
#include "utils.h"
#include "db.h"
#include "comm.h"
#include "interpreter.h"
#include "improved-edit.h"
#include "dg_scripts.h"
#include "modify.h"

/** Append @p src to the char ARRAY @p dst, stopping at sizeof(dst) and always
 * terminating. @p dst must be a real array: for a pointer, sizeof() would be
 * the pointer's size. Evaluates @p dst three times: pass a plain array name.
 * The append can truncate without a word, so callers that cannot lose text
 * check the length afterwards (format_text() does). */
#define STRNCAT_BOUNDED(dst, src) \
  strncat((dst), (src), sizeof(dst) - strlen(dst) - 1)


/* Return 0 for no range, 1 for one line, 2 for a range, -1 on
 * invalid/overflowing input. Whitespace around the dash is permitted. */
static int parse_editor_line_range(const char *text, int *low, int *high)
{
  const char *p = text;
  char *end;
  long first, last;
  if (!p || !low || !high)
    return -1;
  while (isspace((unsigned char)*p))
    p++;
  if (!*p)
    return 0;
  errno = 0;
  first = strtol(p, &end, 10);
  if (p == end || errno == ERANGE || first < INT_MIN || first > INT_MAX)
    return -1;
  p = end;
  while (isspace((unsigned char)*p))
    p++;
  *low = (int)first;
  if (!*p)
    return 1;
  if (*p != '-')
    return -1;
  p++;
  while (isspace((unsigned char)*p))
    p++;
  errno = 0;
  last = strtol(p, &end, 10);
  if (p == end || errno == ERANGE || last < INT_MIN || last > INT_MAX)
    return -1;
  while (isspace((unsigned char)*end))
    end++;
  if (*end)
    return -1;
  *high = (int)last;
  return 2;
}

void send_editor_help(struct descriptor_data *d)
{
  if (using_improved_editor)
    write_to_output(d, "Instructions: /s to save, /h for more options.\r\n");
  else
    write_to_output(d, "Instructions: Type @ on a line by itself to end.\r\n");
}

#if CONFIG_IMPROVED_EDITOR

int improved_editor_execute(struct descriptor_data *d, char *str)
{
  char actions[MAX_INPUT_LENGTH];

  if (*str != '/')
    return STRINGADD_OK;

  /* str is the player's input line. A bare "/" has no command letter, and
   * str + 2 would then point past its terminator into stale buffer bytes. */
  if (!str[1]) {
    write_to_output(d, "Invalid option.\r\n");
    *str = '\0';
    return STRINGADD_ACTION;
  }

  strlcpy(actions, str + 2, sizeof(actions));
  *str = '\0';

  switch (str[1]) {
  case 'a':
    return STRINGADD_ABORT;
  case 'c':
    if (*(d->str)) {
      free(*d->str);
      *(d->str) = NULL;
      write_to_output(d, "Current buffer cleared.\r\n");
    } else
      write_to_output(d, "Current buffer empty.\r\n");
    break;
  case 'd':
    parse_edit_action(PARSE_DELETE, actions, d);
    break;
  case 'e':
    parse_edit_action(PARSE_EDIT, actions, d);
    break;
  case 'f':
    if (*(d->str))
      parse_edit_action(PARSE_FORMAT, actions, d);
    else
      write_to_output(d, "Current buffer empty.\r\n");
    break;
  case 'i':
    if (*(d->str))
      parse_edit_action(PARSE_INSERT, actions, d);
    else
      write_to_output(d, "Current buffer empty.\r\n");
    break;
  case 'h':
    parse_edit_action(PARSE_HELP, actions, d);
    break;
  case 'l':
    if (*d->str)
      parse_edit_action(PARSE_LIST_NORM, actions, d);
    else
      write_to_output(d, "Current buffer empty.\r\n");
    break;
  case 'n':
    if (*d->str)
      parse_edit_action(PARSE_LIST_NUM, actions, d);
    else
      write_to_output(d, "Current buffer empty.\r\n");
    break;
  case 'r':
    parse_edit_action(PARSE_REPLACE, actions, d);
    break;
  case 's':
    return STRINGADD_SAVE;
  case 't':
    parse_edit_action(PARSE_TOGGLE, actions, d);
    break;
  default:
    write_to_output(d, "Invalid option.\r\n");
    break;
  }
  return STRINGADD_ACTION;
}

/* Handle some editor commands. */
void parse_edit_action(int command, char *string, struct descriptor_data *d)
{
  int indent = 0, rep_all = 0, flags = 0, replaced, i, line_low, line_high, j = 0;
  unsigned int total_len;
  char *s, *t, temp, *c;
  char buf[MAX_STRING_LENGTH];
  char buf2[MAX_STRING_LENGTH - 1];
  int range_result;

  switch (command) {
  case PARSE_HELP:
    write_to_output(d,
            "Editor command formats: /<letter>\r\n\r\n"
            "/a         -  aborts editor\r\n"
            "/c         -  clears buffer\r\n"
            "/d#[-#]    -  deletes line #, or lines #-#\r\n"
            "/e# <text> -  changes the line at # with <text>\r\n"
            "/f         -  formats text\r\n"
            "/fi        -  indented formatting of text\r\n"
            "/h         -  list text editor commands\r\n"
            "/i# <text> -  inserts <text> before line #\r\n"
            "/l         -  lists buffer\r\n"
            "/n         -  lists buffer with line numbers\r\n"
            "/r 'a' 'b' -  replace 1st occurence of text <a> in buffer with text <b>\r\n"
            "/ra 'a' 'b'-  replace all occurences of text <a> within buffer with text <b>\r\n"
            "              usage: /r[a] 'pattern' 'replacement'\r\n"
            "/t         -  toggles '@' and tabs\r\n"
            "/s         -  saves text\r\n");
    break;
  case PARSE_TOGGLE:
    if (!*d->str) {
      write_to_output(d, "No string.\r\n");        
      break;
    }
    bool has_at = FALSE;
    for (c = *d->str; *c; ++c) {
      if (*c == '@') {
        if (*(++c) != '@') {
          has_at = TRUE;
          break;
        }
      }
    }
    if (has_at) {
      parse_at(*d->str);
      write_to_output(d, "Toggling (at) into (tab) Characters...\r\n");  
    } else {
      parse_tab(*d->str);
      write_to_output(d, "Toggling (tab) into (at) Characters...\r\n"); 
    }
  break;
  case PARSE_FORMAT:
    if (STATE(d) == CON_TRIGEDIT) {
      write_to_output(d, "Script %sformatted.\r\n", format_script(d) ? "": "not ");
      return;
    }
    while (isalpha(string[j]) && j < 2) {
      if (string[j++] == 'i' && !indent) {
        indent = TRUE;
        flags += FORMAT_INDENT;
      }
    }
    range_result = parse_editor_line_range((indent ? string + 1 : string), &line_low, &line_high);
    switch (range_result)
    {
    case 0:
      line_low = 1;
      line_high = 999999;
      break;
    case 1:
      line_high = line_low;
      break;
    case -1:
      write_to_output(d, "Invalid line range.\r\n");
      return;
    case 2:
      if (line_high < line_low) {
        write_to_output(d, "That range is invalid.\r\n");
        return;
      }
    break;
    }
    /* in case line_low is negative or zero */
    line_low = MAX(1, line_low);

    if (format_text(d->str, flags, d, d->max_str, line_low, line_high))
      write_to_output(d, "Text formatted with%s indent.\r\n", (indent ? "" : "out"));
    break;
  case PARSE_REPLACE:
    while (isalpha(string[j]) && j < 2)
      if (string[j++] == 'a' && !indent)
        rep_all = 1;

    if ((s = strtok(string, "'")) == NULL) {
      write_to_output(d, "Invalid format.\r\n");
      return;
    } else if ((s = strtok(NULL, "'")) == NULL) {
      write_to_output(d, "Target string must be enclosed in single quotes.\r\n");
      return;
    } else if ((t = strtok(NULL, "'")) == NULL) {
      write_to_output(d, "No replacement string.\r\n");
      return;
    } else if ((t = strtok(NULL, "'")) == NULL) {
      write_to_output(d, "Replacement string must be enclosed in single quotes.\r\n");
      return;
      /*wb's fix for empty buffer replacement crashing */
    } else if ((!*d->str)) {
      return;
    } else if ((total_len = ((strlen(t) - strlen(s)) + strlen(*d->str))) <= d->max_str) {
      if ((replaced = replace_str(d->str, s, t, rep_all, d->max_str)) > 0) {
        write_to_output(d, "Replaced %d occurence%sof '%s' with '%s'.\r\n", replaced, ((replaced != 1) ? "s " : " "), s, t);
      } else if (replaced == 0) {
        write_to_output(d, "String '%s' not found.\r\n", s);
      } else
        write_to_output(d, "ERROR: Replacement string causes buffer overflow, aborted replace.\r\n");
    } else
      write_to_output(d, "Not enough space left in buffer.\r\n");
    break;
  case PARSE_DELETE:
    range_result = parse_editor_line_range(string, &line_low, &line_high);
    switch (range_result) {
    case -1:
      write_to_output(d, "Invalid line range.\r\n");
      return;
    case 0:
      write_to_output(d, "You must specify a line number or range to delete.\r\n");
      return;
    case 1:
      line_high = line_low;
      break;
    case 2:
      if (line_high < line_low) {
        write_to_output(d, "That range is invalid.\r\n");
        return;
      }
      break;
    }

    i = 1;
    total_len = 1;
    if ((s = *d->str) == NULL) {
      write_to_output(d, "Buffer is empty.\r\n");
      return;
    } else if (line_low > 0) {
      while (s && i < line_low)
        if ((s = strchr(s, '\n')) != NULL) {
          i++;
          s++;
        }
      if (s == NULL || i < line_low) {
        write_to_output(d, "Line(s) out of range; not deleting.\r\n");
        return;
      }
      t = s;
      while (s && i < line_high)
        if ((s = strchr(s, '\n')) != NULL) {
          i++;
          total_len++;
          s++;
        }
      if (s && (s = strchr(s, '\n')) != NULL) {
        while (*(++s))
          *(t++) = *s;
      } else
        total_len--;
      *t = '\0';
      {
        /* Deleting the last line leaves "" in a 3-byte block. Keep the two
         * spare bytes every RECREATE here allows for, and NUL them: they
         * held stale text, and the insert below used to read past the
         * terminator into them (and beyond). */
        size_t len = strlen(*d->str);

        RECREATE(*d->str, char, len + 3);
        memset(*d->str + len, '\0', 3);
      }

      write_to_output(d, "%d line%sdeleted.\r\n", total_len, (total_len != 1 ? "s " : " "));
    } else {
      write_to_output(d, "Invalid, line numbers to delete must be higher than 0.\r\n");
      return;
    }
    break;
  case PARSE_LIST_NORM:
    /* Note: Rv's buf, buf1, buf2, and arg variables are defined to 32k so they
     * are ok for what we do here. */
    *buf = '\0';
    if (*string)
      switch (parse_editor_line_range(string, &line_low, &line_high)) {
      case -1:
        write_to_output(d, "Invalid line range.\r\n");
        return;
      case 0:
        line_low = 1;
        line_high = 999999;
        break;
      case 1:
        line_high = line_low;
        break;
    } else {
      line_low = 1;
      line_high = 999999;
    }

    if (line_low < 1) {
      write_to_output(d, "Line numbers must be greater than 0.\r\n");
      return;
    } else if (line_high < line_low) {
      write_to_output(d, "That range is invalid.\r\n");
      return;
    }
    *buf = '\0';
    if (line_high < 999999 || line_low > 1)
      tb_snprintf(buf, sizeof(buf), "Current buffer range [%d - %d]:\r\n", line_low, line_high);
    i = 1;
    total_len = 0;
    s = *d->str;
    while (s && (i < line_low))
      if ((s = strchr(s, '\n')) != NULL) {
        i++;
        s++;
      }
    if (i < line_low || s == NULL) {
      write_to_output(d, "Line(s) out of range; no buffer listing.\r\n");
      return;
    }
    t = s;
    while (s && i <= line_high)
      if ((s = strchr(s, '\n')) != NULL) {
        i++;
        total_len++;
        s++;
      }
    if (s) {
      temp = *s;
      *s = '\0';
      STRNCAT_BOUNDED(buf, t);
      *s = temp;
    } else
      STRNCAT_BOUNDED(buf, t);
    /* This is kind of annoying...but some people like it. */
    tb_snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "\r\n%d line%sshown.\r\n", total_len, (total_len != 1) ? "s " : " ");
    page_string(d, buf, TRUE);
    break;
  case PARSE_LIST_NUM:
    /* Note: Rv's buf, buf1, buf2, and arg variables are defined to 32k so they
     * are probably ok for what we do here. */
    *buf = '\0';
    if (*string)
      switch (parse_editor_line_range(string, &line_low, &line_high)) {
      case -1:
        write_to_output(d, "Invalid line range.\r\n");
        return;
      case 0:
        line_low = 1;
        line_high = 999999;
        break;
      case 1:
        line_high = line_low;
        break;
    } else {
      line_low = 1;
      line_high = 999999;
    }

    if (line_low < 1) {
      write_to_output(d, "Line numbers must be greater than 0.\r\n");
      return;
    }
    if (line_high < line_low) {
      write_to_output(d, "That range is invalid.\r\n");
      return;
    }
    *buf = '\0';
    i = 1;
    total_len = 0;
    s = *d->str;
    while (s && i < line_low)
      if ((s = strchr(s, '\n')) != NULL) {
        i++;
        s++;
      }
    if (i < line_low || s == NULL) {
      write_to_output(d, "Line(s) out of range; no buffer listing.\r\n");
      return;
    }
    t = s;
    while (s && i <= line_high)
      if ((s = strchr(s, '\n')) != NULL) {
        i++;
        total_len++;
        s++;
        temp = *s;
        *s = '\0';
        char buf3[13];
        tb_snprintf(buf3, sizeof(buf3), "%4d: ", (i - 1));
        STRNCAT_BOUNDED(buf, buf3);
        STRNCAT_BOUNDED(buf, t);
        *s = temp;
        t = s;
      }
    if (s && t) {
      temp = *s;
      *s = '\0';
      STRNCAT_BOUNDED(buf, t);
      *s = temp;
    } else if (t)
      STRNCAT_BOUNDED(buf, t);

    page_string(d, buf, TRUE);
    break;

  case PARSE_INSERT:
    half_chop(string, buf, buf2);
    if (*buf == '\0') {
      write_to_output(d, "You must specify a line number before which to insert text.\r\n");
      return;
    }
    line_low = tb_parse_int_or_zero(buf);
    STRNCAT_BOUNDED(buf2, "\r\n");

    i = 1;
    *buf = '\0';
    if ((s = *d->str) == NULL) {
      write_to_output(d, "Buffer is empty, nowhere to insert.\r\n");
      return;
    }
    if (line_low > 0) {
      while (s && (i < line_low))
      if ((s = strchr(s, '\n')) != NULL) {
        i++;
        s++;
      }
      if (i < line_low || s == NULL) {
        write_to_output(d, "Line number out of range; insert aborted.\r\n");
        return;
      }
      temp = *s;
      *s = '\0';
      /* s is the insertion point. When it is the terminator (an empty
       * buffer, or inserting after the last line) there is no tail, and
       * s + 1 lies past the string, possibly past the block: strlen(s + 1)
       * was an ASan heap-buffer-overflow after "/d1" emptied the buffer. */
      if ((strlen(*d->str) + strlen(buf2) + (temp ? strlen(s + 1) : 0) + 3) > d->max_str) {
        *s = temp;
        write_to_output(d, "Insert text pushes buffer over maximum size, insert aborted.\r\n");
        return;
      }
      if (*d->str && **d->str)
        STRNCAT_BOUNDED(buf, *d->str);
      *s = temp;
      STRNCAT_BOUNDED(buf, buf2);
      if (*s)
        STRNCAT_BOUNDED(buf, s);
      RECREATE(*d->str, char, strlen(buf) + 3);

      memcpy(*d->str, buf, strlen(buf) + 1);
      write_to_output(d, "Line inserted.\r\n");
    } else {
      write_to_output(d, "Line number must be higher than 0.\r\n");
      return;
    }
    break;

  case PARSE_EDIT:
    half_chop(string, buf, buf2);
    if (*buf == '\0') {
      write_to_output(d, "You must specify a line number at which to change text.\r\n");
      return;
    }
    line_low = tb_parse_int_or_zero(buf);
    STRNCAT_BOUNDED(buf2, "\r\n");

    i = 1;
    *buf = '\0';
    if ((s = *d->str) == NULL) {
      write_to_output(d, "Buffer is empty, nothing to change.\r\n");
      return;
    }
    if (line_low > 0) {
      /* Loop through the text counting \n characters until we get to the line. */
      while (s && i < line_low)
        if ((s = strchr(s, '\n')) != NULL) {
          i++;
          s++;
        }
      /* Make sure that there was a THAT line in the text. */
      if (s == NULL || i < line_low) {
        write_to_output(d, "Line number out of range; change aborted.\r\n");
        return;
      }
      /* If s is the same as *d->str that means I'm at the beginning of the
       * message text and I don't need to put that into the changed buffer. */
      if (s != *d->str) {
        /* First things first .. we get this part into the buffer. */
        temp = *s;
        *s = '\0';
        /* Put the first 'good' half of the text into storage. */
        STRNCAT_BOUNDED(buf, *d->str);
        *s = temp;
      }
      /* Put the new 'good' line into place. */
      STRNCAT_BOUNDED(buf, buf2);
      if ((s = strchr(s, '\n')) != NULL) {
        /* This means that we are at the END of the line, we want out of there,
         * but we want s to point to the beginning of the line. AFTER the line 
         * we want edited. */
        s++;
        /* Now put the last 'good' half of buffer into storage. */
        STRNCAT_BOUNDED(buf, s);
      }
      /* Check for buffer overflow. */
      if (strlen(buf) > d->max_str) {
        write_to_output(d, "Change causes new length to exceed buffer maximum size, aborted.\r\n");
        return;
      }
      /* Change the size of the REAL buffer to fit the new text. */
      RECREATE(*d->str, char, strlen(buf) + 3);
      memcpy(*d->str, buf, strlen(buf) + 1);
      write_to_output(d, "Line changed.\r\n");
    } else {
      write_to_output(d, "Line number must be higher than 0.\r\n");
      return;
    }
    break;
  default:
    write_to_output(d, "Invalid option.\r\n");
    mudlog(BRF, LVL_IMPL, TRUE, "SYSERR: invalid command passed to parse_edit_action");
    return;
  }
}

/* Re-formats message type formatted char *. (for strings edited with d->str)
 * (mostly olc and mail). */
/**
 * Re-flow lines @p low..@p high of the edit buffer to PAGE_WIDTH ("/f").
 *
 * @param ptr_string The edit buffer (*d->str): NUL-terminated, malloc'd.
 *        It is reallocated: the caller must not keep the old pointer. Its
 *        text may come from world files, so it need not end in a newline and
 *        may end in a lone '\t' colour code (dropped and reported,
 *        "format-color").
 * @param mode   FORMAT_INDENT to indent the first line.
 * @param d      The editor, for messages.
 * @param maxlen Hard cap for the result, including the final "\r\n" and NUL
 *               (d->max_str). Must not exceed MAX_STRING_LENGTH, the size of
 *               the local work buffers; a larger d->max_str is refused.
 * @param low,high 1-based line range.
 * @return 1 when the buffer was reformatted, 0 when it was left unchanged.
 *
 * The working copy str[] is filled with strncpy(..., sizeof(str) - 1) and is
 * only terminated because the text is shorter than d->max_str <=
 * MAX_STRING_LENGTH; keep that precondition if you change either bound.
 */
int format_text(char **ptr_string, int mode, struct descriptor_data *d, unsigned int maxlen, int low, int high)
{
  int line_chars, cap_next = TRUE, cap_next_next = FALSE, color_chars = 0, i, pass_line = 0;
  char *flow, *start = NULL, temp, *work;
  char formatted[MAX_STRING_LENGTH] = "";
  char str[MAX_STRING_LENGTH];

  /* Fix memory overrun. */
  if (d->max_str > MAX_STRING_LENGTH) {
    TB_ERROR_MSG("editor", "format-max", "edit-buffer",
                 "desc %d max_str %zu exceeds the %d-byte work buffers; "
                 "format refused", d->desc_num, d->max_str, MAX_STRING_LENGTH);
    return 0;
  }

  /* XXX: Want to make sure the string doesn't grow either... */
  if (*ptr_string == NULL)
    return 0;

  /* The scan below writes into the text it walks (capitals, temporary
   * terminators). Walk a copy, so a format that is refused leaves the
   * player's buffer exactly as it was. */
  if ((work = strdup(*ptr_string)) == NULL) {
    TB_ERROR_ERRNO("editor", "format-copy", "edit-buffer",
                   "desc %d: copying %zu bytes failed", d->desc_num,
                   strlen(*ptr_string));
    return 0;
  }
  flow = work;

  strncpy(str, flow, sizeof(str) - 1);

  for (i = 0; i < low - 1; i++) {
    start = strtok(str, "\n");
    if (!start) {
      write_to_output(d, "There aren't that many lines!\r\n");
      free(work);
      return 0;
    }
    strlcat(formatted, start, sizeof(formatted));
    strlcat(formatted, "\n", sizeof(formatted));
    if ((flow = strstr(flow, "\n")) == NULL) {
      write_to_output(d, "There aren't that many lines!\r\n");
      free(work);
      return 0;
    }
    strncpy(str, ++flow, sizeof(str) - 1);
  }

  if (IS_SET(mode, FORMAT_INDENT)) {
    STRNCAT_BOUNDED(formatted, "   ");
    line_chars = 3;
  } else {
    line_chars = 0;
  }

  while (*flow && i < high) {
    while (*flow && strchr("\n\r\f\v ", *flow)) {
      if (*flow == '\n' && !pass_line)
        if (i++ >= high) {
          pass_line = 1;
          break;
        }
      flow++;
    }

    if (*flow) {
      start = flow;
      while (*flow && !strchr("\n\r\f\v .?!", *flow)) {
        /* A '\t' colour code owns the next byte, so skipping it moves flow
         * by two. Text loaded by fread_string() can end in a lone '\t'
         * (parse_at() turns a trailing '@' into one); stepping over its
         * missing code byte would leave flow past the terminator, and the
         * loops and writes below would touch bytes outside the buffer.
         * The introducer displays nothing, so drop it and say so. */
        if (*flow == '\t' && !flow[1]) {
          TB_WARN_MSG("editor", "format-color", "edit-buffer",
                      "dangling tab colour code at offset %td dropped; "
                      "desc %d", flow - work, d->desc_num);
          write_to_output(d, "Dropped a colour code with nothing after it "
                             "at the end of the text.\r\n");
          *flow = '\0';
          break;
        }
        if (*flow == '\t') {
          if (*(flow + 1) == '\t')
            color_chars++;
          else if (*(flow + 1) == '[')
            color_chars += 7;
          else
            color_chars += 2;
          flow++;
        }
        flow++;
      }

      if (cap_next_next) {
        cap_next_next = FALSE;
        cap_next = TRUE;
      }

      /* This is so that if we stopped on a sentence, we move off the sentence 
       * delimiter. strchr() matches the terminator itself, so test *flow
       * first: text that ends without a newline stops here on the NUL. */
      while (*flow && strchr(".!?", *flow)) {
        cap_next_next = TRUE;
        flow++;
      }

      /* Special case: if we're at the end of the last line, and the last
       * character is a delimiter, the flow++ above will have *flow pointing
       * to the \r (or \n) character after the delimiter. Thus *flow will be 
       * non-null, and an extra (blank) line might be added erroneously. We 
       * fix it by skipping the newline characters in between. - Welcor */
      if (*flow && strchr("\n\r", *flow)) {
        *flow = '\0';  /* terminate 'start' string */
        flow++;        /* we know this is safe     */
        if (*flow == '\n' && i++ >= high)
          pass_line = 1;

        while (*flow && strchr("\n\r", *flow) && !pass_line) {
          flow++;      /* skip to next non-delimiter */
          if (*flow == '\n' && i++ >= high)
            pass_line = 1;
        }
        temp = *flow;  /* save this char             */
     } else {
        temp = *flow;
        *flow = '\0';
      }

      if (line_chars + strlen(start) + 1 - color_chars > PAGE_WIDTH) {
        STRNCAT_BOUNDED(formatted, "\r\n");
        line_chars = 0;
        color_chars = count_color_chars(start);
      }

      if (!cap_next) {
        if (line_chars > 0) {
          STRNCAT_BOUNDED(formatted, " ");
          line_chars++;
        }
      } else {
        cap_next = FALSE;
        CAP(start);
      }

      line_chars += strlen(start);
      STRNCAT_BOUNDED(formatted, start);

      *flow = temp;
    }

    if (cap_next_next && *flow) {
      if (line_chars + 3 - color_chars > PAGE_WIDTH) {
        STRNCAT_BOUNDED(formatted, "\r\n");
        line_chars = 0;
        color_chars = count_color_chars(start);
      } else if (*flow == '\"' || *flow == '\'') {
        char buf[MAX_STRING_LENGTH - 1];
        tb_snprintf(buf, sizeof(buf), "%c  ", *flow);
        STRNCAT_BOUNDED(formatted, buf);
        flow++;
        line_chars++;
      } else {
        STRNCAT_BOUNDED(formatted, "  ");
        line_chars += 2;
      }
    }
  }
  if (*flow)
    STRNCAT_BOUNDED(formatted, "\r\n");
  STRNCAT_BOUNDED(formatted, flow);
  if (!*flow)
    STRNCAT_BOUNDED(formatted, "\r\n");

  /* Formatting adds sentence spacing and line breaks, so the result can be
   * longer than the text. Cutting it to maxlen (or to the work buffer, which
   * the appends above stop at) would silently lose the end of the text;
   * leave the buffer as it was instead. */
  free(work);
  if (strlen(formatted) + 1 > maxlen || strlen(formatted) + 1 >= sizeof(formatted)) {
    write_to_output(d, "Formatted text would be too long; text not changed.\r\n");
    return 0;
  }
  RECREATE(*ptr_string, char, strlen(formatted) + 3); /* +3: room for "\r\n" */
  memcpy(*ptr_string, formatted, strlen(formatted) + 1);
  return 1;
}

/**
 * Replace the first (or every, @p rep_all) @p pattern in *@p string ("/r").
 *
 * @param string   The edit buffer: NUL-terminated, malloc'd. On success it
 *                 is reallocated, so the caller must not keep the old pointer.
 * @param pattern,replacement Player-typed, NUL-terminated; pattern non-empty.
 * @param max_size Hard cap for the result including the NUL (d->max_str).
 * @return Number of replacements; 0 if none (buffer unchanged); -1 if the
 *         result would exceed @p max_size (buffer unchanged).
 */
int replace_str(char **string, char *pattern, char *replacement, int rep_all, unsigned int max_size)
{
  const char *scan, *hit;
  char *result, *out;
  size_t current_len, result_len, pattern_len, replacement_len, room, growth;
  int count = 0, i;

  if (!string || !*string || !pattern || !*pattern || !replacement || max_size < 2)
    return 0;

  current_len = strlen(*string);
  pattern_len = strlen(pattern);
  replacement_len = strlen(replacement);

  /* Size the whole result before changing anything. The edit buffer is
   * either fully rewritten or left exactly as it was: the old loop cut the
   * buffer at the failing match on overflow, and its strncat() bounds
   * dropped the last byte of a result that exactly filled max_size, both
   * without a word. */
  for (scan = *string; count < INT_MAX && (hit = strstr(scan, pattern)) != NULL;
       scan = hit + pattern_len) {
    count++;
    if (!rep_all)
      break;
  }
  if (count == 0)
    return 0;

  room = current_len < max_size - 1 ? max_size - 1 - current_len : 0;
  if (replacement_len >= pattern_len) {
    growth = replacement_len - pattern_len;
    if (growth && (size_t)count > room / growth)
      return -1;
    result_len = current_len + (size_t)count * growth;
  } else
    result_len = current_len - (size_t)count * (pattern_len - replacement_len);
  if (result_len > max_size - 1)
    return -1;

  CREATE(result, char, result_len + 3); /* +3: room for a "\r\n" append */
  out = result;
  for (scan = *string, i = 0; i < count && (hit = strstr(scan, pattern)) != NULL; i++) {
    memcpy(out, scan, (size_t)(hit - scan));
    out += hit - scan;
    memcpy(out, replacement, replacement_len);
    out += replacement_len;
    scan = hit + pattern_len;
  }
  memcpy(out, scan, strlen(scan) + 1);

  free(*string);
  *string = result;
  return count;
}

#endif
