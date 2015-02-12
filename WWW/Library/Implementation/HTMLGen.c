/*
 * $LynxId: HTMLGen.c,v 1.36 2011/06/11 12:10:02 tom Exp $
 *
 *		HTML Generator
 *		==============
 *
 *	This version of the HTML object sends HTML markup to the output stream.
 *
 * Bugs:	Line wrapping is not done at all.
 *		All data handled as PCDATA.
 *		Should convert old XMP, LISTING and PLAINTEXT to PRE.
 *
 *	It is not obvious to me right now whether the HEAD should be generated
 *	from the incomming data or the anchor.	Currently it is from the former
 *	which is cleanest.
 */

#define HTSTREAM_INTERNAL 1

#include <HTUtils.h>

#define BUFFER_SIZE    200	/* Line buffer attempts to make neat breaks */
#define MAX_CLEANNESS	20

/* Implements:
*/
#include <HTMLGen.h>

#include <HTMLDTD.h>
#include <HTStream.h>
#include <SGML.h>
#include <HTFormat.h>

#ifdef USE_COLOR_STYLE
#include <LYCharUtils.h>
#include <AttrList.h>
#include <LYHash.h>
#include <LYStyle.h>
#endif

#include <LYGlobalDefs.h>
#include <LYCurses.h>
#include <LYLeaks.h>

#ifdef USE_COLOR_STYLE
char class_string[TEMPSTRINGSIZE];

static char *Style_className = NULL;
static char myHash[128];
static int hcode;
#endif

/*		HTML Object
 *		-----------
 */
struct _HTStream {
    const HTStreamClass *isa;
    HTStream *target;
    HTStreamClass targetClass;	/* COPY for speed */
};

struct _HTStructured {
    const HTStructuredClass *isa;
    HTStream *target;
    HTStreamClass targetClass;	/* COPY for speed */

    char buffer[BUFFER_SIZE + 1];	/* 1for NL */
    int buffer_maxchars;
    char *write_pointer;
    char *line_break[MAX_CLEANNESS + 1];
    int cleanness;
    BOOL overflowed;
    BOOL delete_line_break_char[MAX_CLEANNESS + 1];
    BOOL preformatted;
    BOOL escape_specials;
    BOOL in_attrval;
#ifdef USE_COLOR_STYLE
    HText *text;
#endif
};

/*	Flush Buffer
 *	------------
 */

static void flush_breaks(HTStructured * me)
{
    int i;

    for (i = 0; i <= MAX_CLEANNESS; i++) {
	me->line_break[i] = NULL;
    }
}

static void HTMLGen_flush(HTStructured * me)
{
    (*me->targetClass.put_block) (me->target,
				  me->buffer,
				  (int) (me->write_pointer - me->buffer));
    me->write_pointer = me->buffer;
    flush_breaks(me);
    me->cleanness = 0;
    me->delete_line_break_char[0] = NO;
}

#ifdef USE_COLOR_STYLE
/*
 * We need to flush our buffer each time before we effect a color style change,
 * this also relies on the subsequent stage not doing any buffering - this is
 * currently true, in cases where it matters the target stream should be the
 * HTPlain converter.  The flushing currently prevents reasonable line breaking
 * in lines with tags.  Since color styles help visual scanning of displayed
 * source lines, and long lines are wrapped in GridText anyway, this is
 * probably acceptable (or even A Good Thing - more to see in one screenful). 
 * The pointer to the HText structure is initialized here before we effect the
 * first style change.  Getting it from the global HTMainText variable isn't
 * very clean, since it relies on the fact that HText_new() has already been
 * called for the current stream stack's document by the time we start
 * processing the first element; we rely on HTMLGenerator's callers
 * (HTMLParsedPresent in particular) to guarantee this when it matters. 
 * Normally the target stream will have been setup by HTPlainPresent, which
 * does what we need in this respect.  (A check whether we have the right
 * output stream could be done by checking that targetClass.name is
 * "PlainPresenter" or similar.)
 *
 * All special color style handling is only done if LYPreparsedSource is set. 
 * We could always do it for displaying source generated by an internal
 * gateway, but this makes the rule more simple for the user:  color styles are
 * applied to html source only with the -preparsed flag.  - kw
 */
static void do_cstyle_flush(HTStructured * me)
{
    if (!me->text && LYPreparsedSource) {
	me->text = HTMainText;
    }
    if (me->text) {
	HTMLGen_flush(me);
    }
}
#endif /* COLOR_STYLE */

/*	Weighted optional line break
 *
 *	We keep track of all the breaks for when we chop the line
 */

static void allow_break(HTStructured * me, int new_cleanness, int dlbc)
{
    if (dlbc && me->write_pointer == me->buffer)
	dlbc = NO;
    me->line_break[new_cleanness] =
	dlbc ? me->write_pointer - 1	/* Point to space */
	: me->write_pointer;	/* point to gap */
    me->delete_line_break_char[new_cleanness] = (BOOLEAN) dlbc;
    if (new_cleanness >= me->cleanness &&
	(me->overflowed || me->line_break[new_cleanness] > me->buffer))
	me->cleanness = new_cleanness;
}

/*	Character handling
 *	------------------
 *
 *	The tricky bits are the line break handling.  This attempts
 *	to synchrononise line breaks on sentence or phrase ends.  This
 *	is important if one stores SGML files in a line-oriented code
 *	repository, so that if a small change is made, line ends don't
 *	shift in a ripple-through to apparently change a large part of the
 *	file.  We give extra "cleanness" to spaces appearing directly
 *	after periods (full stops), [semi]colons and commas.
 *	   This should make the source files easier to read and modify
 *	by hand, too, though this is not a primary design consideration. TBL
 */
static void HTMLGen_put_character(HTStructured * me, int c)
{
    if (me->escape_specials && UCH(c) < 32) {
	if (c == HT_NON_BREAK_SPACE || c == HT_EN_SPACE ||
	    c == LY_SOFT_HYPHEN) {	/* recursion... */
	    HTMLGen_put_character(me, '&');
	    HTMLGen_put_character(me, '#');
	    HTMLGen_put_character(me, 'x');
	    switch (c) {
	    case HT_NON_BREAK_SPACE:	/* &#xA0; */
		HTMLGen_put_character(me, 'A');
		HTMLGen_put_character(me, '0');
		break;
	    case HT_EN_SPACE:	/* &#x2002; */
		HTMLGen_put_character(me, '2');
		HTMLGen_put_character(me, '0');
		HTMLGen_put_character(me, '0');
		HTMLGen_put_character(me, '2');
		break;
	    case LY_SOFT_HYPHEN:	/* &#xAD; */
		HTMLGen_put_character(me, 'A');
		HTMLGen_put_character(me, 'D');
		break;
	    }
	    c = ';';
	}
    }

    *me->write_pointer++ = (char) c;

    if (c == '\n') {
	HTMLGen_flush(me);
	return;
    }

    /* Figure our whether we can break at this point
     */
    if ((!me->preformatted && (c == ' ' || c == '\t'))) {
	int new_cleanness = 3;

	if (me->write_pointer > (me->buffer + 1)) {
	    char delims[5];
	    char *p;

	    strcpy(delims, ",;:.");	/* @@ english bias */
	    p = strchr(delims, me->write_pointer[-2]);
	    if (p)
		new_cleanness = (int) (p - delims + 6);
	    if (!me->in_attrval)
		new_cleanness += 10;
	}
	allow_break(me, new_cleanness, YES);
    }

    /*
     * Flush buffer out when full, or whenever the line is over the nominal
     * maximum and we can break at all
     */
    if (me->write_pointer >= me->buffer + me->buffer_maxchars ||
	(me->overflowed && me->cleanness)) {
	if (me->cleanness) {
	    char line_break_char = me->line_break[me->cleanness][0];
	    char *saved = me->line_break[me->cleanness];

	    if (me->delete_line_break_char[me->cleanness])
		saved++;
	    me->line_break[me->cleanness][0] = '\n';
	    (*me->targetClass.put_block) (me->target,
					  me->buffer,
					  (int) (me->line_break[me->cleanness] -
						 me->buffer + 1));
	    me->line_break[me->cleanness][0] = line_break_char;
	    {			/* move next line in */
		char *p = saved;
		char *q;

		for (q = me->buffer; p < me->write_pointer;)
		    *q++ = *p++;
	    }
	    me->cleanness = 0;
	    /* Now we have to check whether ther are any perfectly good breaks
	     * which weren't good enough for the last line but may be good
	     * enough for the next
	     */
	    {
		int i;

		for (i = 0; i <= MAX_CLEANNESS; i++) {
		    if (me->line_break[i] != NULL &&
			me->line_break[i] > saved) {
			me->line_break[i] = me->line_break[i] -
			    (saved - me->buffer);
			me->cleanness = i;
		    } else {
			me->line_break[i] = NULL;
		    }
		}
	    }

	    me->delete_line_break_char[0] = 0;
	    me->write_pointer = me->write_pointer - (saved - me->buffer);
	    me->overflowed = NO;

	} else {
	    (*me->targetClass.put_block) (me->target,
					  me->buffer,
					  me->buffer_maxchars);
	    me->write_pointer = me->buffer;
	    flush_breaks(me);
	    me->overflowed = YES;
	}
    }
}

/*	String handling
 *	---------------
 */
static void HTMLGen_put_string(HTStructured * me, const char *s)
{
    const char *p;

    for (p = s; *p; p++)
	HTMLGen_put_character(me, *p);
}

static void HTMLGen_write(HTStructured * me, const char *s,
			  int l)
{
    const char *p;

    for (p = s; p < (s + l); p++)
	HTMLGen_put_character(me, *p);
}

/*	Start Element
 *	-------------
 *
 * Within the opening tag, there may be spaces and the line may be broken at
 * these spaces.
 */
static int HTMLGen_start_element(HTStructured * me, int element_number,
				 const BOOL *present,
				 const char **value,
				 int charset GCC_UNUSED,
				 char **insert GCC_UNUSED)
{
    int i;
    BOOL was_preformatted = me->preformatted;
    HTTag *tag = &HTML_dtd.tags[element_number];

#if defined(USE_COLOR_STYLE)
    char *title = NULL;
    char *title_tmp = NULL;

    if (LYPreparsedSource) {
	/*
	 * Same logic as in HTML_start_element, copied from there.  - kw
	 */
	HTSprintf(&Style_className, ";%s", HTML_dtd.tags[element_number].name);
	strcpy(myHash, HTML_dtd.tags[element_number].name);
	if (class_string[0]) {
	    int len = (int) strlen(myHash);

	    sprintf(myHash + len, ".%.*s", (int) sizeof(myHash) - len - 2, class_string);
	    HTSprintf(&Style_className, ".%s", class_string);
	}
	class_string[0] = '\0';
	strtolower(myHash);
	hcode = hash_code(myHash);
	strtolower(Style_className);

	if (TRACE_STYLE) {
	    fprintf(tfp, "CSSTRIM:%s -> %d", myHash, hcode);
	    if (hashStyles[hcode].code != hcode) {
		char *rp = strrchr(myHash, '.');

		fprintf(tfp, " (undefined) %s\n", myHash);
		if (rp) {
		    int hcd;

		    *rp = '\0';	/* trim the class */
		    hcd = hash_code(myHash);
		    fprintf(tfp, "CSS:%s -> %d", myHash, hcd);
		    if (hashStyles[hcd].code != hcd)
			fprintf(tfp, " (undefined) %s\n", myHash);
		    else
			fprintf(tfp, " ca=%d\n", hashStyles[hcd].color);
		}
	    } else
		fprintf(tfp, " ca=%d\n", hashStyles[hcode].color);
	}

	if (displayStyles[element_number + STARTAT].color > -2) {
	    CTRACE2(TRACE_STYLE,
		    (tfp, "CSSTRIM: start_element: top <%s>\n",
		     HTML_dtd.tags[element_number].name));
	    do_cstyle_flush(me);
	    HText_characterStyle(me->text, hcode, 1);
	}
    }
#endif /* USE_COLOR_STYLE */
    me->preformatted = YES;	/* free text within tags */
    HTMLGen_put_character(me, '<');
    HTMLGen_put_string(me, tag->name);
    if (present) {
	BOOL had_attr = NO;

	for (i = 0; i < tag->number_of_attributes; i++) {
	    if (present[i]) {
		had_attr = YES;
		HTMLGen_put_character(me, ' ');
		allow_break(me, 11, YES);
#ifdef USE_COLOR_STYLE
		/*
		 * Try to mimic HTML_start_element's special handling for
		 * HTML_LINK.  If applicable, color the displayed attribute /
		 * value pairs differently.  - kw
		 */
		if (LYPreparsedSource &&
		    element_number == HTML_LINK && !title &&
		    present[HTML_LINK_CLASS] && *value[HTML_LINK_CLASS] &&
		    !present[HTML_LINK_REV] &&
		    (present[HTML_LINK_REL] || present[HTML_LINK_HREF])) {
		    if (present[HTML_LINK_TITLE] && *value[HTML_LINK_TITLE]) {
			StrAllocCopy(title, value[HTML_LINK_TITLE]);
			LYTrimHead(title);
			LYTrimTail(title);
		    }
		    if ((!title || *title == '\0') && present[HTML_LINK_REL]) {
			StrAllocCopy(title, value[HTML_LINK_REL]);
		    }
		    if (title && *title) {
			HTSprintf0(&title_tmp, "link.%s.%s",
				   value[HTML_LINK_CLASS], title);
			CTRACE2(TRACE_STYLE,
				(tfp, "CSSTRIM:link=%s\n", title_tmp));

			do_cstyle_flush(me);
			HText_characterStyle(me->text, hash_code(title_tmp), 1);
		    }
		}
#endif
		HTMLGen_put_string(me, tag->attributes[i].name);
		if (value[i]) {
		    me->preformatted = was_preformatted;
		    me->in_attrval = YES;
		    if (strchr(value[i], '"') == NULL) {
			HTMLGen_put_string(me, "=\"");
			HTMLGen_put_string(me, value[i]);
			HTMLGen_put_character(me, '"');
		    } else if (strchr(value[i], '\'') == NULL) {
			HTMLGen_put_string(me, "='");
			HTMLGen_put_string(me, value[i]);
			HTMLGen_put_character(me, '\'');
		    } else {	/* attribute value has both kinds of quotes */
			const char *p;

			HTMLGen_put_string(me, "=\"");
			for (p = value[i]; *p; p++) {
			    if (*p != '"') {
				HTMLGen_put_character(me, *p);
			    } else {
				HTMLGen_put_string(me, "&#34;");
			    }
			}
			HTMLGen_put_character(me, '"');
		    }
		    me->preformatted = YES;
		    me->in_attrval = NO;
		}
	    }
	}
#ifdef USE_COLOR_STYLE
	if (had_attr && LYPreparsedSource && element_number == HTML_LINK) {
	    /*
	     * Clean up after special HTML_LINK handling - kw
	     */
	    if (title && *title) {
		do_cstyle_flush(me);
		HText_characterStyle(me->text, hash_code(title_tmp), 0);
		FREE(title_tmp);
	    }
	    FREE(title);
	}
#endif
	if (had_attr)
	    allow_break(me, 12, NO);
    }
    HTMLGen_put_string(me, ">");	/* got rid of \n LJM */

    /*
     * Make very specific HTML assumption that PRE can't be nested!
     */
    me->preformatted = (BOOL) ((element_number == HTML_PRE)
			       ? YES
			       : was_preformatted);

    /*
     * Can break after element start.
     */
    if (!me->preformatted && tag->contents != SGML_EMPTY) {
	if (HTML_dtd.tags[element_number].contents == SGML_ELEMENT)
	    allow_break(me, 15, NO);
	else
	    allow_break(me, 2, NO);
    }
#if defined(USE_COLOR_STYLE)
    /*
     * Same logic as in HTML_start_element, copied from there.  - kw
     */

    /* end really empty tags straight away */
    if (LYPreparsedSource && ReallyEmptyTagNum(element_number)) {
	CTRACE2(TRACE_STYLE,
		(tfp, "STYLE:begin_element:ending EMPTY element style\n"));
	do_cstyle_flush(me);
	HText_characterStyle(me->text, hcode, STACK_OFF);
	TrimColorClass(HTML_dtd.tags[element_number].name,
		       Style_className, &hcode);
    }
#endif /* USE_COLOR_STYLE */
    if (element_number == HTML_OBJECT && tag->contents == SGML_LITTERAL) {
	/*
	 * These conditions only approximate the ones used in HTML.c.  Let our
	 * SGML parser know that further content is to be parsed normally not
	 * literally.  - kw
	 */
	if (!present) {
	    return HT_PARSER_OTHER_CONTENT;
	} else if (!present[HTML_OBJECT_DECLARE] &&
		   !(present[HTML_OBJECT_NAME] &&
		     value[HTML_OBJECT_NAME] && *value[HTML_OBJECT_NAME])) {
	    if (present[HTML_OBJECT_SHAPES] ||
		!(present[HTML_OBJECT_USEMAP] &&
		  value[HTML_OBJECT_USEMAP] && *value[HTML_OBJECT_USEMAP]))
		return HT_PARSER_OTHER_CONTENT;
	}
    }
    return HT_OK;
}

/*		End Element
 *		-----------
 *
 * When we end an element, the style must be returned to that in effect before
 * that element.  Note that anchors (etc?) don't have an associated style, so
 * that we must scan down the stack for an element with a defined style.  (In
 * fact, the styles should be linked to the whole stack not just the top one.)
 * TBL 921119
 */
static int HTMLGen_end_element(HTStructured * me, int element_number,
			       char **insert GCC_UNUSED)
{
    if (!me->preformatted &&
	HTML_dtd.tags[element_number].contents != SGML_EMPTY) {
	/*
	 * Can break before element end.
	 */
	if (HTML_dtd.tags[element_number].contents == SGML_ELEMENT)
	    allow_break(me, 14, NO);
	else
	    allow_break(me, 1, NO);
    }
    HTMLGen_put_string(me, "</");
    HTMLGen_put_string(me, HTML_dtd.tags[element_number].name);
    HTMLGen_put_character(me, '>');
    if (element_number == HTML_PRE) {
	me->preformatted = NO;
    }
#ifdef USE_COLOR_STYLE
    /*
     * Same logic as in HTML_end_element, copied from there.  - kw
     */
    TrimColorClass(HTML_dtd.tags[element_number].name,
		   Style_className, &hcode);

    if (LYPreparsedSource && !ReallyEmptyTagNum(element_number)) {
	CTRACE2(TRACE_STYLE,
		(tfp, "STYLE:end_element: ending non-EMPTY style\n"));
	do_cstyle_flush(me);
	HText_characterStyle(me->text, hcode, STACK_OFF);
    }
#endif /* USE_COLOR_STYLE */
    return HT_OK;
}

/*		Expanding entities
 *		------------------
 *
 */
static int HTMLGen_put_entity(HTStructured * me, int entity_number)
{
    int nent = (int) HTML_dtd.number_of_entities;

    HTMLGen_put_character(me, '&');
    if (entity_number < nent) {
	HTMLGen_put_string(me, HTML_dtd.entity_names[entity_number]);
    }
    HTMLGen_put_character(me, ';');
    return HT_OK;
}

/*	Free an HTML object
 *	-------------------
 *
 */
static void HTMLGen_free(HTStructured * me)
{
    (*me->targetClass.put_character) (me->target, '\n');
    HTMLGen_flush(me);
    (*me->targetClass._free) (me->target);	/* ripple through */
#ifdef USE_COLOR_STYLE
    FREE(Style_className);
#endif
    FREE(me);
}

static void PlainToHTML_free(HTStructured * me)
{
    HTMLGen_end_element(me, HTML_PRE, 0);
    HTMLGen_free(me);
}

static void HTMLGen_abort(HTStructured * me, HTError e GCC_UNUSED)
{
    HTMLGen_free(me);
#ifdef USE_COLOR_STYLE
    FREE(Style_className);
#endif
}

static void PlainToHTML_abort(HTStructured * me, HTError e GCC_UNUSED)
{
    PlainToHTML_free(me);
}

/*	Structured Object Class
 *	-----------------------
 */
static const HTStructuredClass HTMLGeneration =		/* As opposed to print etc */
{
    "HTMLGen",
    HTMLGen_free,
    HTMLGen_abort,
    HTMLGen_put_character, HTMLGen_put_string, HTMLGen_write,
    HTMLGen_start_element, HTMLGen_end_element,
    HTMLGen_put_entity
};

/*	Subclass-specific Methods
 *	-------------------------
 */
HTStructured *HTMLGenerator(HTStream *output)
{
    HTStructured *me = (HTStructured *) malloc(sizeof(*me));

    if (me == NULL)
	outofmem(__FILE__, "HTMLGenerator");

    assert(me != NULL);

    me->isa = &HTMLGeneration;

    me->target = output;
    me->targetClass = *me->target->isa;		/* Copy pointers to routines for speed */

    me->write_pointer = me->buffer;
    flush_breaks(me);
    me->line_break[0] = me->buffer;
    me->cleanness = 0;
    me->overflowed = NO;
    me->delete_line_break_char[0] = NO;
    me->preformatted = NO;
    me->in_attrval = NO;

    /*
     * For what line length should we attempt to wrap ?  - kw
     */
    if (!LYPreparsedSource) {
	me->buffer_maxchars = 80;	/* work as before - kw */
    } else if (dump_output_width > 1) {
	me->buffer_maxchars = dump_output_width;	/* try to honor -width - kw */
    } else if (dump_output_immediately) {
	me->buffer_maxchars = 80;	/* try to honor -width - kw */
    } else {
	me->buffer_maxchars = (LYcolLimit - 1);
	if (me->buffer_maxchars < 38)	/* too narrow, let GridText deal */
	    me->buffer_maxchars = 40;
    }
    if (me->buffer_maxchars > 900)	/* likely not true - kw */
	me->buffer_maxchars = 78;
    if (me->buffer_maxchars > BUFFER_SIZE)	/* must not be larger! */
	me->buffer_maxchars = BUFFER_SIZE - 2;

    /*
     * If dump_output_immediately is set, there likely isn't anything after
     * this stream to interpret the Lynx special chars.  Also if they get
     * displayed via HTPlain, that will probably make non-breaking space chars
     * etc.  invisible.  So let's translate them to numerical character
     * references.  For debugging purposes we'll use the new hex format.
     */
    me->escape_specials = LYPreparsedSource;

#ifdef USE_COLOR_STYLE
    me->text = NULL;		/* Will be initialized when first needed. - kw */
    FREE(Style_className);
    class_string[0] = '\0';
#endif /* COLOR_STYLE */

    return me;
}

/*	Stream Object Class
 *	-------------------
 *
 *	This object just converts a plain text stream into HTML
 *	It is officially a structured strem but only the stream bits exist.
 *	This is just the easiest way of typecasting all the routines.
 */
static const HTStructuredClass PlainToHTMLConversion =
{
    "plaintexttoHTML",
    HTMLGen_free,
    PlainToHTML_abort,
    HTMLGen_put_character,
    HTMLGen_put_string,
    HTMLGen_write,
    NULL,			/* Structured stuff */
    NULL,
    NULL
};

/*	HTConverter from plain text to HTML Stream
 *	------------------------------------------
 */
HTStream *HTPlainToHTML(HTPresentation *pres GCC_UNUSED,
			HTParentAnchor *anchor GCC_UNUSED,
			HTStream *sink)
{
    HTStructured *me = (HTStructured *) malloc(sizeof(*me));

    if (me == NULL)
	outofmem(__FILE__, "PlainToHTML");

    assert(me != NULL);

    me->isa = (const HTStructuredClass *) &PlainToHTMLConversion;

    /*
     * Copy pointers to routines for speed.
     */
    me->target = sink;
    me->targetClass = *me->target->isa;
    me->write_pointer = me->buffer;
    flush_breaks(me);
    me->cleanness = 0;
    me->overflowed = NO;
    me->delete_line_break_char[0] = NO;
    /* try to honor -width - kw */
    me->buffer_maxchars = (dump_output_width > 1 ?
			   dump_output_width : 80);

    HTMLGen_put_string(me, "<HTML>\n<BODY>\n<PRE>\n");
    me->preformatted = YES;
    me->escape_specials = NO;
    me->in_attrval = NO;
    return (HTStream *) me;
}
