// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef USERS_H
#define USERS_H

#include "aoxcommand.h"


class ListUsers
    : public AoxCommand
{
public:
    ListUsers( StringList * );
    void execute();

private:
    class Query * q;
};


class CreateUser
    : public AoxCommand
{
public:
    CreateUser( StringList * );
    void execute();

private:
    class CreateUserData * d;
};


class DeleteUser
    : public AoxCommand
{
public:
    DeleteUser( StringList * );
    void execute();

private:
    class DeleteUserData * d;
};


class ChangePassword
    : public AoxCommand
{
public:
    ChangePassword( StringList * );
    void execute();

private:
    class Query * q;
};


class ChangeUsername
    : public AoxCommand
{
public:
    ChangeUsername( StringList * );
    void execute();

private:
    class ChangeUsernameData * d;
};


class ChangeAddress
    : public AoxCommand
{
public:
    ChangeAddress( StringList * );
    void execute();

private:
    class ChangeAddressData * d;
};


#endif