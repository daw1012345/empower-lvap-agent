// -*- c-basic-offset: 4 -*-
/*
 * lexert.{cc,hh} -- configuration file parser for tools
 * Eddie Kohler
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 * Copyright (c) 2000 Mazu Networks, Inc.
 * Copyright (c) 2001-2003 International Computer Science Institute
 * Copyright (c) 2004 Regents of the University of California
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>

#include "lexert.hh"
#include "lexertinfo.hh"
#include "routert.hh"
#include <click/confparse.hh>
#include <click/variableenv.hh>
#include <ctype.h>
#include <stdlib.h>

static LexerTInfo *stub_lexinfo = 0;

LexerT::LexerT(ErrorHandler *errh, bool ignore_line_directives)
  : _data(0), _len(0), _pos(0), _lineno(1),
    _ignore_line_directives(ignore_line_directives),
    _tpos(0), _tfull(0), _router(0), _base_type_map(0), _errh(errh)
{
    if (!_errh)
	_errh = ErrorHandler::default_handler();
    if (!stub_lexinfo)
	stub_lexinfo = new LexerTInfo;
    _lexinfo = stub_lexinfo;
    clear();
}

LexerT::~LexerT()
{
    clear();
}

void
LexerT::reset(const String &data, const String &filename)
{
    clear();
  
    _big_string = data;
    _data = _big_string.data();
    _len = _big_string.length();

    if (!filename)
	_filename = "line ";
    else if (filename.back() != ':' && !isspace(filename.back()))
	_filename = filename + ":";
    else
	_filename = filename;
    _original_filename = _filename;
    _lineno = 1;
}

void
LexerT::clear()
{
    if (_router)
	_router->unuse();
    _router = new RouterT;
    _router->use();		// hold a reference to the router

    _big_string = "";
    // _data was freed by _big_string
    _data = 0;
    _len = 0;
    _pos = 0;
    _filename = "";
    _lineno = 0;
    _tpos = 0;
    _tfull = 0;

    _base_type_map.clear();
    _anonymous_offset = 0;
}

void
LexerT::set_lexinfo(LexerTInfo *li)
{
    _lexinfo = (li ? li : stub_lexinfo);
}


// LEXING: LOWEST LEVEL

String
LexerT::remaining_text() const
{
  return _big_string.substring(_pos);
}

void
LexerT::set_remaining_text(const String &s)
{
  _big_string = s;
  _data = s.data();
  _pos = 0;
  _len = s.length();
}

unsigned
LexerT::skip_line(unsigned pos)
{
  _lineno++;
  for (; pos < _len; pos++)
    if (_data[pos] == '\n')
      return pos + 1;
    else if (_data[pos] == '\r') {
      if (pos < _len - 1 && _data[pos+1] == '\n')
	return pos + 2;
      else
	return pos + 1;
    }
  _lineno--;
  return _len;
}

unsigned
LexerT::skip_slash_star(unsigned pos)
{
  for (; pos < _len; pos++)
    if (_data[pos] == '\n')
      _lineno++;
    else if (_data[pos] == '\r') {
      if (pos < _len - 1 && _data[pos+1] == '\n') pos++;
      _lineno++;
    } else if (_data[pos] == '*' && pos < _len - 1 && _data[pos+1] == '/')
      return pos + 2;
  return _len;
}

unsigned
LexerT::skip_backslash_angle(unsigned pos)
{
  for (; pos < _len; pos++)
    if (_data[pos] == '\n')
      _lineno++;
    else if (_data[pos] == '\r') {
      if (pos < _len - 1 && _data[pos+1] == '\n') pos++;
      _lineno++;
    } else if (_data[pos] == '/' && pos < _len - 1) {
      if (_data[pos+1] == '/')
	pos = skip_line(pos + 2) - 1;
      else if (_data[pos+1] == '*')
	pos = skip_slash_star(pos + 2) - 1;
    } else if (_data[pos] == '>')
      return pos + 1;
  return _len;
}

unsigned
LexerT::skip_quote(unsigned pos, char endc)
{
  for (; pos < _len; pos++)
    if (_data[pos] == '\n')
      _lineno++;
    else if (_data[pos] == '\r') {
      if (pos < _len - 1 && _data[pos+1] == '\n') pos++;
      _lineno++;
    } else if (_data[pos] == '\\' && endc == '\"' && pos < _len - 1) {
      if (_data[pos+1] == '<')
	pos = skip_backslash_angle(pos + 2) - 1;
      else if (_data[pos+1] == '\"')
	pos++;
    } else if (_data[pos] == endc)
      return pos + 1;
  return _len;
}

unsigned
LexerT::process_line_directive(unsigned pos)
{
  const char *data = _data;
  unsigned len = _len;
  unsigned first_pos = pos;
  
  for (pos++; pos < len && (data[pos] == ' ' || data[pos] == '\t'); pos++)
    /* nada */;
  if (pos < len - 4 && data[pos] == 'l' && data[pos+1] == 'i'
      && data[pos+2] == 'n' && data[pos+3] == 'e'
      && (data[pos+4] == ' ' || data[pos+4] == '\t')) {
    for (pos += 5; pos < len && (data[pos] == ' ' || data[pos] == '\t'); pos++)
      /* nada */;
  }
  if (pos >= len || !isdigit(data[pos])) {
    // complain about bad directive
    lerror(first_pos, pos, "unknown preprocessor directive");
    return skip_line(pos);
  } else if (_ignore_line_directives)
    return skip_line(pos);
  
  // parse line number
  for (_lineno = 0; pos < len && isdigit(data[pos]); pos++)
    _lineno = _lineno * 10 + data[pos] - '0';
  _lineno--;			// account for extra line
  
  for (; pos < len && (data[pos] == ' ' || data[pos] == '\t'); pos++)
    /* nada */;
  if (pos < len && data[pos] == '\"') {
    // parse filename
    unsigned first_in_filename = pos;
    for (pos++; pos < len && data[pos] != '\"' && data[pos] != '\n' && data[pos] != '\r'; pos++)
      if (data[pos] == '\\' && pos < len - 1 && data[pos+1] != '\n' && data[pos+1] != '\r')
	pos++;
    _filename = cp_unquote(_big_string.substring(first_in_filename, pos - first_in_filename) + "\":");
    // an empty filename means return to the input file's name
    if (_filename == ":")
      _filename = _original_filename;
  }

  // reach end of line
  for (; pos < len && data[pos] != '\n' && data[pos] != '\r'; pos++)
    /* nada */;
  if (pos < len - 1 && data[pos] == '\r' && data[pos+1] == '\n')
    pos++;
  return pos;
}

Lexeme
LexerT::next_lexeme()
{
  unsigned pos = _pos;
  while (true) {
    while (pos < _len && isspace(_data[pos])) {
      if (_data[pos] == '\n')
	_lineno++;
      else if (_data[pos] == '\r') {
	if (pos < _len - 1 && _data[pos+1] == '\n') pos++;
	_lineno++;
      }
      pos++;
    }
    unsigned opos = pos;
    if (pos >= _len) {
      _pos = _len;
      return Lexeme();
    } else if (_data[pos] == '/' && pos < _len - 1) {
      if (_data[pos+1] == '/')
	pos = skip_line(pos + 2);
      else if (_data[pos+1] == '*')
	pos = skip_slash_star(pos + 2);
      else
	break;
      _lexinfo->notify_comment(opos, pos);
    } else if (_data[pos] == '#' && (pos == 0 || _data[pos-1] == '\n' || _data[pos-1] == '\r')) {
      pos = process_line_directive(pos);
      _lexinfo->notify_line_directive(opos, pos);
    } else
      break;
  }
  
  unsigned word_pos = pos;
  
  // find length of current word
  if (isalnum(_data[pos]) || _data[pos] == '_' || _data[pos] == '@') {
   more_word_characters:
    pos++;
    while (pos < _len && (isalnum(_data[pos]) || _data[pos] == '_' || _data[pos] == '@'))
      pos++;
    if (pos < _len - 1 && _data[pos] == '/' && (isalnum(_data[pos+1]) || _data[pos+1] == '_' || _data[pos+1] == '@'))
      goto more_word_characters;
    _pos = pos;
    String word = _big_string.substring(word_pos, pos - word_pos);
    if (word.length() == 16 && word == "connectiontunnel") {
      _lexinfo->notify_keyword(word, word_pos, pos);
      return Lexeme(lexTunnel, word, word_pos);
    } else if (word.length() == 12 && word == "elementclass") {
      _lexinfo->notify_keyword(word, word_pos, pos);
      return Lexeme(lexElementclass, word, word_pos);
    } else if (word.length() == 7 && word == "require") {
      _lexinfo->notify_keyword(word, word_pos, pos);
      return Lexeme(lexRequire, word, word_pos);
    } else
      return Lexeme(lexIdent, word, word_pos);
  }

  // check for variable
  if (_data[pos] == '$') {
    pos++;
    while (pos < _len && (isalnum(_data[pos]) || _data[pos] == '_'))
      pos++;
    if (pos > word_pos + 1) {
      _pos = pos;
      return Lexeme(lexVariable, _big_string.substring(word_pos, pos - word_pos), word_pos);
    } else
      pos--;
  }

  if (pos < _len - 1) {
    if (_data[pos] == '-' && _data[pos+1] == '>') {
      _pos = pos + 2;
      return Lexeme(lexArrow, _big_string.substring(word_pos, 2), word_pos);
    } else if (_data[pos] == ':' && _data[pos+1] == ':') {
      _pos = pos + 2;
      return Lexeme(lex2Colon, _big_string.substring(word_pos, 2), word_pos);
    } else if (_data[pos] == '|' && _data[pos+1] == '|') {
      _pos = pos + 2;
      return Lexeme(lex2Bar, _big_string.substring(word_pos, 2), word_pos);
    }
  }
  if (pos < _len - 2 && _data[pos] == '.' && _data[pos+1] == '.' && _data[pos+2] == '.') {
    _pos = pos + 3;
    return Lexeme(lex3Dot, _big_string.substring(word_pos, 3), word_pos);
  }
  
  _pos = pos + 1;
  return Lexeme(_data[word_pos], _big_string.substring(word_pos, 1), word_pos);
}

Lexeme
LexerT::lex_config()
{
  unsigned config_pos = _pos;
  unsigned pos = _pos;
  unsigned paren_depth = 1;
  
  for (; pos < _len; pos++)
    if (_data[pos] == '(')
      paren_depth++;
    else if (_data[pos] == ')') {
      paren_depth--;
      if (!paren_depth) break;
    } else if (_data[pos] == '\n')
      _lineno++;
    else if (_data[pos] == '\r') {
      if (pos < _len - 1 && _data[pos+1] == '\n') pos++;
      _lineno++;
    } else if (_data[pos] == '/' && pos < _len - 1) {
      if (_data[pos+1] == '/')
	pos = skip_line(pos + 2) - 1;
      else if (_data[pos+1] == '*')
	pos = skip_slash_star(pos + 2) - 1;
    } else if (_data[pos] == '\'' || _data[pos] == '\"')
      pos = skip_quote(pos + 1, _data[pos]) - 1;
    else if (_data[pos] == '\\' && pos < _len - 1 && _data[pos+1] == '<')
      pos = skip_backslash_angle(pos + 2) - 1;
  
  _pos = pos;
  _lexinfo->notify_config_string(config_pos, pos);
  return Lexeme(lexConfig, _big_string.substring(config_pos, pos - config_pos),
		config_pos);
}

String
LexerT::lexeme_string(int kind)
{
  char buf[12];
  if (kind == lexIdent)
    return "identifier";
  else if (kind == lexVariable)
    return "variable";
  else if (kind == lexArrow)
    return "'->'";
  else if (kind == lex2Colon)
    return "'::'";
  else if (kind == lex2Bar)
    return "'||'";
  else if (kind == lex3Dot)
    return "'...'";
  else if (kind == lexTunnel)
    return "'connectiontunnel'";
  else if (kind == lexElementclass)
    return "'elementclass'";
  else if (kind == lexRequire)
    return "'require'";
  else if (kind >= 32 && kind < 127) {
    sprintf(buf, "'%c'", kind);
    return buf;
  } else {
    sprintf(buf, "'\\%03d'", kind);
    return buf;
  }
}


// LEXING: MIDDLE LEVEL (WITH PUSHBACK)

const Lexeme &
LexerT::lex()
{
  int p = _tpos;
  if (_tpos == _tfull) {
    _tcircle[p] = next_lexeme();
    _tfull = (_tfull + 1) % TCIRCLE_SIZE;
  }
  _tpos = (_tpos + 1) % TCIRCLE_SIZE;
  return _tcircle[p];
}

void
LexerT::unlex(const Lexeme &t)
{
  _tpos = (_tpos + TCIRCLE_SIZE - 1) % TCIRCLE_SIZE;
  _tcircle[_tpos] = t;
  assert(_tfull != _tpos);
}

bool
LexerT::expect(int kind, bool report_error)
{
    // Never adds anything to '_tcircle'. This requires a nonobvious
    // implementation.
    if (_tpos != _tfull) {
	if (_tcircle[_tpos].is(kind)) {
	    _tpos = (_tpos + 1) % TCIRCLE_SIZE;
	    return true;
	}
	if (report_error)
	    lerror(_tcircle[_tpos], "expected %s", lexeme_string(kind).cc());
    } else {
	unsigned old_pos = _pos;
	if (next_lexeme().is(kind))
	    return true;
	if (report_error)
	    lerror(old_pos, _pos, "expected %s", lexeme_string(kind).cc());
	_pos = old_pos;
    }
    return false;
}

int
LexerT::next_pos() const
{
    if (_tpos != _tfull)
	return _tcircle[_tpos].pos1();
    else
	return _pos;
}


// ERRORS

String
LexerT::landmark() const
{
    return _filename + String(_lineno);
}

void
LexerT::vlerror(int pos1, int pos2, const String &lm, const char *format, va_list val)
{
    String text = _errh->make_text(ErrorHandler::ERR_ERROR, format, val);
    _lexinfo->notify_error(text, pos1, pos2);
    text = _errh->decorate_text(ErrorHandler::ERR_ERROR, String(), lm, text);
    _errh->handle_text(ErrorHandler::ERR_ERROR, text);
}

int
LexerT::lerror(int pos1, int pos2, const char *format, ...)
{
    va_list val;
    va_start(val, format);
    vlerror(pos1, pos2, landmark(), format, val);
    va_end(val);
    return -1;
}

int
LexerT::lerror(const Lexeme &t, const char *format, ...)
{
    va_list val;
    va_start(val, format);
    vlerror(t.pos1(), t.pos2(), landmark(), format, val);
    va_end(val);
    return -1;
}


// ELEMENT TYPES

ElementClassT *
LexerT::element_type(const Lexeme &t) const
{
    assert(t.is(lexIdent));
    String name = t.string();
    ElementClassT *type = _router->declared_type(name);
    if (!type)
	type = _base_type_map[name];
    if (type)
	_lexinfo->notify_class_reference(type, t.pos1(), t.pos2());
    return type;
}

ElementClassT *
LexerT::force_element_type(const Lexeme &t)
{
    assert(t.is(lexIdent));
    String name = t.string();
    ElementClassT *type = _router->declared_type(name);
    if (!type)
	type = _base_type_map[name];
    if (!type) {
	if (_router->eindex(name) >= 0)
	    lerror(t, "'%s' was previously used as an element name", name.c_str());
	type = ElementClassT::base_type(name);
	_base_type_map.insert(name, type);
    }
    _lexinfo->notify_class_reference(type, t.pos1(), t.pos2());
    return type;
}


// ELEMENTS

String
LexerT::anon_element_name(const String &class_name) const
{
    int anonymizer = _router->nelements() - _anonymous_offset + 1;
    return ";" + class_name + "@" + String(anonymizer);
}

int
LexerT::make_element(String name, const Lexeme &location, int decl_pos2,
		     ElementClassT *type, const String &conf, const String &lm)
{
    // check 'name' for validity
    for (int i = 0; i < name.length(); i++) {
	bool ok = false;
	for (; i < name.length() && name[i] != '/'; i++)
	    if (!isdigit(name[i]))
		ok = true;
	if (!ok) {
	    lerror(location, "element name '%s' has all-digit component", name.cc());
	    break;
	}
    }
    ElementT *e = _router->get_element(name, type, conf, lm ? lm : landmark());
    _lexinfo->notify_element_declaration(e, location.pos1(), location.pos2(), (decl_pos2 < 0 ? location.pos2() : decl_pos2));
    return e->eindex();
}

int
LexerT::make_anon_element(const Lexeme &what, int decl_pos2,
			  ElementClassT *type, const String &conf,
			  const String &lm)
{
    return make_element(anon_element_name(type->name()), what, decl_pos2, type, conf, lm);
}

void
LexerT::connect(int element1, int port1, int port2, int element2)
{
    if (port1 < 0)
	port1 = 0;
    if (port2 < 0)
	port2 = 0;
    _router->add_connection
	(PortT(_router->element(element1), port1),
	 PortT(_router->element(element2), port2), landmark());
}


// PARSING

bool
LexerT::yport(int &port, int &pos1, int &pos2)
{
    const Lexeme &tlbrack = lex();
    if (!tlbrack.is('[')) {
	unlex(tlbrack);
	return false;
    }
    pos1 = tlbrack.pos1();

    const Lexeme &tword = lex();
    if (tword.is(lexIdent)) {
	String p = tword.string();
	const char *ps = p.cc();
	if (isdigit(ps[0]) || ps[0] == '-')
	    port = strtol(ps, (char **)&ps, 0);
	if (*ps != 0) {
	    lerror(tword, "syntax error: port number should be integer");
	    port = 0;
	}
	expect(']');
	pos2 = next_pos();
	return true;
    } else if (tword.is(']')) {
	lerror(tword, "syntax error: expected port number");
	port = 0;
	pos2 = tword.pos2();
	return true;
    } else {
	lerror(tword, "syntax error: expected port number");
	unlex(tword);
	return false;
    }
}

bool
LexerT::yelement(int &element, bool comma_ok)
{
    Lexeme tname = lex();
    String name;
    ElementClassT *etype;
    int decl_pos2 = -1;

    if (tname.is(lexIdent)) {
	etype = element_type(tname);
	name = tname.string();
	decl_pos2 = tname.pos2();
    } else if (tname.is('{')) {
	etype = ycompound(String(), tname.pos1(), tname.pos1());
	name = etype->name();
	decl_pos2 = next_pos();
    } else {
	unlex(tname);
	return false;
    }
    
    Lexeme configuration;
    String lm;
    const Lexeme &tparen = lex();
    if (tparen.is('(')) {
	lm = landmark();	// report landmark from before config string
	if (!etype)
	    etype = force_element_type(tname);
	configuration = lex_config();
	expect(')');
	decl_pos2 = next_pos();
    } else
	unlex(tparen);

    if (etype)
	element = make_anon_element(tname, decl_pos2, etype, configuration.string(), lm);
    else {
	const Lexeme &t2colon = lex();
	unlex(t2colon);
	if (t2colon.is(lex2Colon) || (t2colon.is(',') && comma_ok)) {
	    ydeclaration(tname);
	    element = _router->eindex(name);
	} else {
	    element = _router->eindex(name);
	    if (element < 0) {
		// assume it's an element type
		etype = force_element_type(tname);
		element = make_anon_element(tname, tname.pos2(), etype, configuration.string(), lm);
	    } else
		_lexinfo->notify_element_reference(_router->element(element), tname.pos1(), tname.pos2());
	}
    }

    return true;
}

void
LexerT::ydeclaration(const Lexeme &first_element)
{
    Vector<Lexeme> decls;
    Lexeme t;

    if (first_element) {
	decls.push_back(first_element);
	goto midpoint;
    }

    while (true) {
	t = lex();
	if (!t.is(lexIdent))
	    lerror(t, "syntax error: expected element name");
	else
	    decls.push_back(t);
    
      midpoint:
	const Lexeme &tsep = lex();
	if (tsep.is(','))
	    /* do nothing */;
	else if (tsep.is(lex2Colon))
	    break;
	else {
	    lerror(tsep, "syntax error: expected '::' or ','");
	    unlex(tsep);
	    return;
	}
    }

    String lm = landmark();
    ElementClassT *etype;
    Lexeme etypet = lex();
    if (etypet.is(lexIdent))
	etype = force_element_type(etypet);
    else if (etypet.is('{'))
	etype = ycompound(String(), etypet.pos1(), etypet.pos1());
    else {
	lerror(etypet, "missing element type in declaration");
	return;
    }

    Lexeme configuration;
    t = lex();
    if (t.is('(')) {
	configuration = lex_config();
	expect(')');
    } else
	unlex(t);

    int decl_pos2 = (decls.size() == 1 ? next_pos() : -1);

    for (int i = 0; i < decls.size(); i++) {
	String name = decls[i].string();
	if (ElementT *old_e = _router->element(name))
	    ElementT::redeclaration_error(_errh, "element", name, landmark(), old_e->landmark());
	else if (_router->declared_type(name) || _base_type_map[name])
	    lerror(decls[i], "class '%s' used as element name", name.c_str());
	else
	    make_element(name, decls[i], decl_pos2, etype, configuration.string(), lm);
    }
}

bool
LexerT::yconnection()
{
    int element1 = -1;
    int port1 = -1, port1_pos1 = -1, port1_pos2 = -1;
    Lexeme t;

    while (true) {
	int element2;
	int port2 = -1, port2_pos1, port2_pos2;

	// get element
	yport(port2, port2_pos1, port2_pos2);
	if (!yelement(element2, element1 < 0)) {
	    if (port1 >= 0)
		lerror(port1_pos1, port1_pos2, "output port useless at end of chain");
	    return element1 >= 0;
	}

	if (element1 >= 0)
	    connect(element1, port1, port2, element2);
	else if (port2 >= 0)
	    lerror(port2_pos1, port2_pos2, "input port useless at start of chain");
    
	port1 = -1;
    
      relex:
	t = lex();
	switch (t.kind()) {
      
	  case ',':
	  case lex2Colon:
	    if (router()->element(element2)->anonymous())
		// type used as name
		lerror(t, "class '%s' used as element name", router()->etype_name(element2).c_str());
	    else
		lerror(t, "syntax error before '%s'", t.string().c_str());
	    goto relex;
      
	  case lexArrow:
	    break;
      
	  case '[':
	    unlex(t);
	    yport(port1, port1_pos1, port1_pos2);
	    goto relex;
      
	  case lexIdent:
	  case '{':
	  case '}':
	  case lex2Bar:
	  case lexTunnel:
	  case lexElementclass:
	  case lexRequire:
	    unlex(t);
	    // FALLTHRU
	  case ';':
	  case lexEOF:
	    if (port1 >= 0)
		lerror(port1_pos1, port1_pos2, "output port useless at end of chain", port1);
	    return true;
      
	  default:
	    lerror(t, "syntax error near '%#s'", t.string().c_str());
	    if (t.kind() >= lexIdent)	// save meaningful tokens
		unlex(t);
	    return true;
      
	}
    
	// have 'x ->'
	element1 = element2;
    }
}

void
LexerT::yelementclass(int pos1)
{
    Lexeme tname = lex();
    String eclass_name;
    if (!tname.is(lexIdent)) {
	unlex(tname);
	lerror(tname, "expected element type name");
    } else {
	String n = tname.string();
	if (_router->eindex(n) >= 0)
	    lerror(tname, "'%s' already used as an element name", n.cc());
	else
	    eclass_name = n;
    }

    Lexeme tnext = lex();
    if (tnext.is('{'))
	(void) ycompound(eclass_name, pos1, tname.pos1());

    else if (tnext.is(lexIdent)) {
	ElementClassT *ec = force_element_type(tnext);
	if (eclass_name) {
	    ElementClassT *new_ec = new SynonymElementClassT(eclass_name, ec, _router);
	    _router->add_declared_type(new_ec, false);
	    _lexinfo->notify_class_declaration(new_ec, false, pos1, tname.pos1(), tnext.pos2());
	}
	
    } else
	lerror(tnext, "syntax error near '%#s'", tnext.string().c_str());
}

void
LexerT::ytunnel()
{
    Lexeme tname1 = lex();
    if (!tname1.is(lexIdent)) {
	unlex(tname1);
	lerror(tname1, "expected tunnel input name");
    }
    
    expect(lexArrow);
  
    Lexeme tname2 = lex();
    if (!tname2.is(lexIdent)) {
	unlex(tname2);
	lerror(tname2, "expected tunnel output name");
    }
  
    if (tname1.is(lexIdent) && tname2.is(lexIdent))
	_router->add_tunnel(tname1.string(), tname2.string(), landmark(), _errh);
}

void
LexerT::ycompound_arguments(RouterT *comptype)
{
  Lexeme t1, t2;
  
  while (1) {
    String vartype, varname;

    // read "IDENTIFIER $VARIABLE" or "$VARIABLE"
    t1 = lex();
    if (t1.is(lexIdent)) {
      t2 = lex();
      if (t2.is(lexVariable)) {
	vartype = t1.string();
	varname = t2.string();
      } else {
	if (comptype->nformals() > 0)
	  lerror(t2, "expected variable");
	unlex(t2);
	unlex(t1);
	break;
      }
    } else if (t1.is(lexVariable))
      varname = t1.string();
    else if (t1.is('|'))
      break;
    else {
      if (comptype->nformals() > 0)
	lerror(t1, "expected variable");
      unlex(t1);
      break;
    }

    comptype->add_formal(varname, vartype);

    const Lexeme &tsep = lex();
    if (tsep.is('|'))
      break;
    else if (!tsep.is(',')) {
      lerror(tsep, "expected ',' or '|'");
      unlex(tsep);
      break;
    }
  }

  // check argument types
  bool positional = true, error = false;
  for (int i = 0; i < comptype->nformals(); i++)
    if (const String &ftype = comptype->formal_types()[i]) {
      positional = false;
      if (ftype == "__REST__") {
	if (i < comptype->nformals() - 1)
	  error = true;
      } else
	for (int j = i + 1; j < comptype->nformals(); j++)
	  if (comptype->formal_types()[j] == ftype) {
	    lerror(t1, "repeated keyword parameter '%s' in compound element", ftype.c_str());
	    break;
	  }
    } else if (!positional)
      error = true;
  if (error)
    lerror(t1, "compound element parameters out of order\n(The correct order is '[positional], [keywords], [__REST__]'.)");
}

ElementClassT *
LexerT::ycompound(String name, int decl_pos1, int name_pos1)
{
    bool anonymous = (name.length() == 0);

    // '{' was already read
    RouterT *old_router = _router;
    int old_offset = _anonymous_offset;

    RouterT *first = 0, *last = 0;
    ElementClassT *extension = 0;

    int pos2 = name_pos1;
    
    while (1) {
	Lexeme dots = lex();
	if (dots.is(lex3Dot)) {
	    // '...' marks an extension type
	    if (anonymous) {
		lerror(dots, "cannot extend anonymous compound element class");
		extension = ElementClassT::base_type("Error");
	    } else {
		extension = force_element_type(Lexeme(lexIdent, name, name_pos1));
		_lexinfo->notify_class_extension(extension, dots.pos1(), dots.pos2());
	    }
	    
	    dots = lex();
	    if (!first || !dots.is('}'))
		lerror(dots.pos1(), dots.pos2(), "'...' should occur last, after one or more compounds");
	    if (dots.is('}') && first)
		break;
	}
	unlex(dots);

	// create a compound
	RouterT *compound_class = new RouterT(name, landmark(), old_router);
	_router = compound_class->cast_router();
	_anonymous_offset = 2;

	ycompound_arguments(compound_class);
	while (ystatement(true))
	    /* nada */;

	compound_class->finish_type(_errh);

	if (last)
	    last->set_overload_type(compound_class);
	else
	    first = compound_class;
	last = compound_class;

	// check for '||' or '}'
	const Lexeme &t = lex();
	if (!t.is(lex2Bar)) {
	    pos2 = t.pos2();
	    break;
	}
    }

    _anonymous_offset = old_offset;
    _router = old_router;

    if (extension)
	last->set_overload_type(extension);
    old_router->add_declared_type(first, anonymous);
    _lexinfo->notify_class_declaration(first, anonymous, decl_pos1, name_pos1, pos2);
    return first;
}

void
LexerT::yrequire()
{
    if (expect('(')) {
	Lexeme requirement = lex_config();
	expect(')');
	// pre-read ';' to make it easier to write parsing extensions
	expect(';', false);

	Vector<String> args;
	String word;
	cp_argvec(requirement.string(), args);
	for (int i = 0; i < args.size(); i++) {
	    Vector<String> words;
	    cp_spacevec(args[i], words);
	    if (words.size() == 0)
		/* do nothing */;
	    else if (!cp_word(words[0], &word))
		lerror(requirement, "bad requirement: not a word");
	    else if (words.size() > 1)
		lerror(requirement, "bad requirement: too many words");
	    else
		_router->add_requirement(word);
	}
    }
}

bool
LexerT::ystatement(bool nested)
{
  const Lexeme &t = lex();
  switch (t.kind()) {
    
   case lexIdent:
   case '[':
   case '{':
    unlex(t);
    yconnection();
    return true;
    
   case lexElementclass:
    yelementclass(t.pos1());
    return true;
    
   case lexTunnel:
    ytunnel();
    return true;

   case lexRequire:
    yrequire();
    return true;

   case ';':
    return true;
    
   case '}':
   case lex2Bar:
    if (!nested)
      goto syntax_error;
    unlex(t);
    return false;
    
   case lexEOF:
    if (nested)
      lerror(t, "expected '}'");
    return false;
    
   default:
   syntax_error:
    lerror(t, "syntax error near '%#s'", t.string().c_str());
    return true;
    
  }
}


// COMPLETION

RouterT *
LexerT::finish()
{
    RouterT *r = _router;
    _router = 0;
    // resolve anonymous element names
    r->deanonymize_elements();
    // returned router has one reference count
    return r;
}

#include <click/vector.cc>
#include <click/hashmap.cc>
