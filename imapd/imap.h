#ifndef __IMAP_H__
#define __IMAP_H__

#include "connection.h"

class Command;
class Mailbox;

class IMAP : public Connection {
public:
    IMAP(int s);
    ~IMAP();

    int react(Event e);

    int parse();
    void addCommand();
    void runCommands();

    enum State { NotAuthenticated, Authenticated, Selected, Logout };
    State state() const;
    void setState( State );

    void setIdle( bool );
    bool idle() const;

    void reserve( Command * );

    Mailbox *mailbox();
    void setMailbox( Mailbox * );

private:
    class IMAPData *d;
};

#endif
