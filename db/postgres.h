#ifndef POSTGRES_H
#define POSTGRES_H

#include "database.h"

class Row;


class Postgres
    : public Database
{
public:
    Postgres();

    bool ready();
    void reserve();
    void release();
    void enqueue( class Query * );
    void execute();

    void react( Event e );

private:
    class PgData *d;

    void authentication( char );
    void backendStartup( char );
    void process( char );
    void unknown( char );
    void error( const String & );

    bool haveMessage();
    Row *composeRow( const class PgDataRow & );
    void processQueue( bool = false );
};


#endif
