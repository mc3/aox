// Copyright 2009 The Archiveopteryx Developers <info@aox.org>

#include "acl.h"

#include "utf.h"
#include "user.h"
#include "query.h"
#include "estring.h"
#include "mailbox.h"
#include "estringlist.h"
#include "permissions.h"
#include "transaction.h"


class AclData
    : public Garbage
{
public:
    AclData()
        : state( 0 ), type( Acl::SetAcl ), mailbox( 0 ),
          permissions( 0 ), user( 0 ), q( 0 ),
          setOp( 0 )
    {}

    int state;
    Acl::Type type;

    UString authid;
    EString rights;
    EString username;

    Mailbox * mailbox;
    Permissions * permissions;
    User * user;
    Query * q;

    int setOp;
};


/*! \class Acl acl.h
    Implements the SETACL/DELETEACL/GETACL/LISTRIGHTS/MYRIGHTS commands
    from RFC 2086.
*/


/*! Creates a new ACL handler of type \a t, which may be SetAcl,
    DeleteAcl, GetAcl, ListRights, or MyRights.
*/

Acl::Acl( Type t )
    : d( new AclData )
{
    d->type = t;
}


void Acl::parse()
{
    space();
    d->mailbox = mailbox();

    if ( d->type == SetAcl || d->type == DeleteAcl ||
         d->type == ListRights )
    {
        space();
        Utf8Codec c;
        d->authid = c.toUnicode( astring() );
        if ( !c.valid() )
            error( Bad, "Parse error in authid: " + c.error() );
    }

    if ( d->type == SetAcl ) {
        space();
        d->rights = astring();
        if ( d->rights[0] == '+' || d->rights[0] == '-' ) {
            if ( d->rights[0] == '+' )
                d->setOp = 1;
            else
                d->setOp = 2;
            d->rights = d->rights.mid( 1 );
        }
    }

    end();
}


void Acl::execute()
{
    if ( d->state == 0 ) {
        if ( d->type == SetAcl &&
             !Permissions::validRights( d->rights ) )
        {
            error( Bad, "Invalid rights" );
            return;
        }

        if ( !( d->type == MyRights || d->type == GetAcl ) ) {
            d->user = new User;
            d->user->setLogin( d->authid );
            d->user->refresh( this );
        }

        d->permissions = new Permissions( d->mailbox, imap()->user(), this );
        d->state = 1;
    }

    if ( d->state == 1 ) {
        if ( !d->permissions->ready() )
            return;
        if ( d->user && d->user->state() == User::Unverified )
            return;

        if ( d->type == MyRights ) {
            respond( "MYRIGHTS " + imapQuoted( d->mailbox ) +
                     " " + d->permissions->string() );
            finish();
            return;
        }

        d->state = 2;
    }

    if ( d->state == 2 ) {
        if ( !d->permissions->allowed( Permissions::Admin ) ) {
            error( No, d->mailbox->name().ascii() + " is not accessible" );
            return;
        }

        if ( d->type == ListRights ) {
            EString s( "LISTRIGHTS " + imapQuoted( d->mailbox ) + " " );
            if ( d->user->id() == d->mailbox->owner() ) {
                s.append( Permissions::all() );
            }
            else {
                EStringList l;
                l.append( "" );

                uint i = 0;
                while ( i < Permissions::NumRights ) {
                    EString * s = new EString;
                    Permissions::Right r = (Permissions::Right)i;
                    s->append( Permissions::rightChar( r ) );
                    l.append( s );
                    i++;
                }

                s.append( l.join( " " ) );
            }
            respond( s );
            finish();
            return;
        }
        else if ( d->type == DeleteAcl ) {
            d->q = new Query( "delete from permissions where "
                              "mailbox=$1 and identifier=$2", this );
            d->q->bind( 1, d->mailbox->id() );
            d->q->bind( 2, d->authid );
            d->q->execute();
        }
        else if ( d->type == GetAcl ) {
            EString s;

            if ( d->mailbox->owner() != 0 ) {
                s.append( "select (select login from users where id=$2) "
                          "as identifier, $3::text as rights "
                          "union select identifier,rights from "
                          "permissions where mailbox=$1" );
                d->q = new Query( s, this );
                d->q->bind( 1, d->mailbox->id() );
                d->q->bind( 2, d->mailbox->owner() );
                d->q->bind( 3, Permissions::all() );
            }
            else {
                s.append( "select * from permissions where mailbox=$1" );
                d->q = new Query( s, this );
                d->q->bind( 1, d->mailbox->id() );
            }

            d->q->execute();
        }
        else if ( d->type == SetAcl ) {
            if ( d->user->id() == d->mailbox->owner() ) {
                // We should presumably not disallow pointless changes
                // that add rights that already exist, but...
                error( No, "can't change owner's rights" );
                return;
            }

            setTransaction( new Transaction( this ) );
            d->q = new Query( "lock permissions in exclusive mode", this );
            transaction()->enqueue( d->q );
            d->q = new Query( "select * from permissions where "
                              "mailbox=$1 and identifier=$2", this );
            d->q->bind( 1, d->mailbox->id() );
            d->q->bind( 2, d->authid );
            transaction()->enqueue( d->q );
            transaction()->execute();
        }

        d->state = 3;
    }

    if ( d->state == 3 ) {
        if ( !d->q->done() )
            return;

        if ( d->type == GetAcl ) {
            EStringList l;
            while ( d->q->hasResults() ) {
                EString * s = new EString;
                Row * r = d->q->nextRow();
                s->append( imapQuoted( r->getEString( "identifier" ),
                                       AString ) );
                s->append( " " );
                s->append( imapQuoted( r->getEString( "rights" ),
                                       AString ) );
                l.append( s );
            }
            respond( "ACL " + imapQuoted( d->mailbox ) + " " + l.join( " " ) );
        }
        else if ( d->type == SetAcl ) {
            if ( d->q->hasResults() ) {
                Row * r = d->q->nextRow();
                Permissions * target =
                    new Permissions( d->mailbox, d->authid,
                                     r->getEString( "rights" ) );
                if ( d->setOp == 0 )
                    target->set( d->rights );
                else if ( d->setOp == 1 )
                    target->allow( d->rights );
                else if ( d->setOp == 2 )
                    target->disallow( d->rights );

                d->q = new Query( "update permissions set rights=$3 where "
                                  "mailbox=$1 and identifier=$2", this );
                d->q->bind( 1, d->mailbox->id() );
                d->q->bind( 2, d->authid );
                d->q->bind( 3, target->string() );
                transaction()->enqueue( d->q );
            }
            else if ( d->setOp != 2 ) {
                d->q = new Query( "insert into permissions "
                                  "(mailbox,identifier,rights) "
                                  "values ($1,$2,$3)", this );
                d->q->bind( 1, d->mailbox->id() );
                d->q->bind( 2, d->authid );
                d->q->bind( 3, d->rights );
                transaction()->enqueue( d->q );
            }
            else {
                // We can't remove rights from a non-existent entry.
                // That sounds OK, but should we return BAD instead?
            }

            d->state = 4;
            transaction()->commit();
        }
    }

    if ( d->state == 4 ) {
        if ( !transaction()->done() )
            return;
        if ( transaction()->failed() )
            error( No, transaction()->error() );
    }

    finish();
}
