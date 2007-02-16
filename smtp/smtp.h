// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef SMTP_H
#define SMTP_H

#include "connection.h"
#include "list.h"


class User;
class String;
class SmtpCommand;


class SMTP 
    : public Connection 
{
public:
    enum Dialect{ Smtp, Lmtp, Submit };

    SMTP( int s, Dialect=Smtp );

    void react( Event e );

    void parse();

    void execute();
    
    enum InputState { Command, Sasl, Chunk, Data };
    InputState inputState() const;
    void setInputState( InputState );

    Dialect dialect() const;

    void setHeloName( const String & );
    String heloName() const;

    class Sieve * sieve() const;

    void reset();

    User * user() const;
    void authenticated( User * );

    void addRecipient( class SmtpRcptTo * );
    List<class SmtpRcptTo> * rcptTo() const;
    
    void setBody( const String & );
    String body() const;

    bool isFirstCommand( SmtpCommand * ) const;

private:
    void parseCommand();

private:
    class SMTPData * d;

};


class LMTP
    : public SMTP
{
public:
    LMTP( int s );
};


class SMTPSubmit
    : public SMTP
{
public:
    SMTPSubmit( int s );
};


#endif
