// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef IMAP_H
#define IMAP_H

#include "saslconnection.h"
#include "list.h"

class String;
class Command;
class ImapSession;


class IMAP
    : public SaslConnection
{
public:
    IMAP( int );

    void parse();
    virtual void react( Event );
    void reserve( Command * );

    enum State { NotAuthenticated, Authenticated, Selected, Logout };
    State state() const;
    void setState( State );

    enum ClientCapability { Condstore, Annotate, NumClientCapabilities };
    bool clientSupports( ClientCapability ) const;
    void setClientSupports( ClientCapability );

    bool idle() const;

    void beginSession( ImapSession * );
    ImapSession *session() const;
    void endSession();

    static void setup();

    List<Command> * commands() const;
    void unblockCommands();

    void sendChallenge( const String & );
    void setUser( class User *, const String & );

    void setPrefersAbsoluteMailboxes( bool );
    bool prefersAbsoluteMailboxes() const;

private:
    class IMAPData *d;

    void addCommand();
    void runCommands();
    void expireCommands();
    void run( Command * );
};


class IMAPS
    : public IMAP
{
public:
    IMAPS( int );

    void finish();

private:
    class IMAPSData * d;
};


#endif
