// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef MANPAGE_H
#define MANPAGE_H

#include "string.h"
#include "list.h"
#include "output.h"


class Function;
class Class;
class Intro;


class ManPage
{
public:
    ManPage( const char * );
    ~ManPage();

    static ManPage * current();

    void startHeadline( Intro * );
    void startHeadline( Class * );
    void startHeadline( Function * );
    void endParagraph();
    void addText( const String & );
    void addArgument( const String & );
    void addFunction( const String &, Function * );
    void addClass( const String &, Class * );

private:
    void output( const String & );
    void addAuthor();
    void addReferences();
    void endPage();

private:
    bool para;
    int fd;
    String directory;
    SortedList<String> references;
};


#endif
