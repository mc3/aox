/****************************************************************************
*																			*
*							Internal Message Handlers						*
*						Copyright Peter Gutmann 1997-2004					*
*																			*
****************************************************************************/

#if defined( INC_ALL )
  #include "crypt.h"
  #include "acl.h"
  #include "kernel.h"
#elif defined( INC_CHILD )
  #include "../crypt.h"
  #include "acl.h"
  #include "kernel.h"
#else
  #include "crypt.h"
  #include "kernel/acl.h"
  #include "kernel/kernel.h"
#endif /* Compiler-specific includes */

/* A pointer to the kernel data block */

static KERNEL_DATA *krnlData = NULL;

/****************************************************************************
*																			*
*								Dependency ACLs								*
*																			*
****************************************************************************/

/* The ACL tables for each object dependency type */

static const FAR_BSS DEPENDENCY_ACL dependencyACLTbl[] = {
	/* Envelopes and sessions can have conventional encryption and MAC 
	   contexts attached */
	MK_DEPACL( OBJECT_TYPE_ENVELOPE, ST_NONE, ST_ENV_ANY, \
			   OBJECT_TYPE_CONTEXT, ST_CTX_CONV | ST_CTX_MAC, ST_NONE ),	
	MK_DEPACL( OBJECT_TYPE_SESSION, ST_NONE, ST_SESS_ANY, \
			   OBJECT_TYPE_CONTEXT, ST_CTX_CONV | ST_CTX_MAC, ST_NONE ),	

	/* PKC contexts can have certs attached and vice versa.  Since the 
	   certificate can change the permissions on the context, we set the
	   DEP_FLAG_UPDATEDEP flag to ensure that the cert permissions get
	   reflected onto the context */
	MK_DEPACL_EX( OBJECT_TYPE_CONTEXT, ST_CTX_PKC, ST_NONE, \
				  OBJECT_TYPE_CERTIFICATE, ST_CERT_ANY, ST_NONE, 
				  DEP_FLAG_UPDATEDEP ),
	MK_DEPACL_EX( OBJECT_TYPE_CERTIFICATE, ST_CERT_ANY, ST_NONE, \
				  OBJECT_TYPE_CONTEXT, ST_CTX_PKC, ST_NONE,
				  DEP_FLAG_UPDATEDEP ),

	/* Contexts can have crypto devices attached */
	MK_DEPACL_EX( OBJECT_TYPE_CONTEXT, ST_CTX_ANY, ST_NONE, \
				  OBJECT_TYPE_DEVICE, ST_DEV_ANY_STD, ST_NONE,
				  DEP_FLAG_MORE ),

	/* Anything can have the system device attached, since all objects not
	   created via crypto devices are created via the system device */
	MK_DEPACL( OBJECT_TYPE_CONTEXT, ST_CTX_ANY, ST_NONE, \
			   OBJECT_TYPE_DEVICE, ST_DEV_SYSTEM, ST_NONE ),	
	MK_DEPACL( OBJECT_TYPE_CERTIFICATE, ST_CERT_ANY, ST_NONE, \
			   OBJECT_TYPE_DEVICE, ST_DEV_SYSTEM, ST_NONE ),	
	MK_DEPACL( OBJECT_TYPE_KEYSET, ST_KEYSET_ANY, ST_NONE, \
			   OBJECT_TYPE_DEVICE, ST_DEV_SYSTEM, ST_NONE ),	
	MK_DEPACL( OBJECT_TYPE_ENVELOPE, ST_NONE, ST_ENV_ANY, \
			   OBJECT_TYPE_DEVICE, ST_DEV_SYSTEM, ST_NONE ),	
	MK_DEPACL( OBJECT_TYPE_SESSION, ST_NONE, ST_SESS_ANY, \
			   OBJECT_TYPE_DEVICE, ST_DEV_SYSTEM, ST_NONE ),	
	MK_DEPACL( OBJECT_TYPE_DEVICE, ST_DEV_ANY_STD, ST_NONE, \
			   OBJECT_TYPE_DEVICE, ST_DEV_SYSTEM, ST_NONE ),	
	MK_DEPACL( OBJECT_TYPE_USER, ST_NONE, ST_USER_ANY, \
			   OBJECT_TYPE_DEVICE, ST_DEV_SYSTEM, ST_NONE ),

	MK_DEPACL_END()
	};

/****************************************************************************
*																			*
*								Utility Functions							*
*																			*
****************************************************************************/

/* Update an action permission.  This implements a ratchet that only allows
   permissions to be made more restrictive after they've initially been set,
   so that once a permission is set to a given level it can't be set back to 
   a less restrictive one (i.e. it's a write-up policy) */

static int updateActionPerms( int currentPerm, const int newPerm )
	{
	int permMask = ACTION_PERM_MASK, i;

	/* For each permission, update its value if the new setting is more
	   restrictive than the current one.  Since smaller values are more
	   restrictive, we can do a simple range comparison and replace the
	   existing value if it's larger than the new one */
	for( i = 0; i < ACTION_PERM_COUNT; i++ )
		{
		if( ( newPerm & permMask ) < ( currentPerm & permMask ) )
			currentPerm = ( currentPerm & ~permMask ) | ( newPerm & permMask );
		permMask <<= ACTION_PERM_BITS;
		}

	/* Postcondition: The new permission is at least as restrictive (or more
	   so) than the old one */
	FORALL( i, 0, ACTION_PERM_COUNT,
			( currentPerm & ( ACTION_PERM_MASK << ( i * ACTION_PERM_BITS ) ) ) <= \
				( newPerm & ( ACTION_PERM_MASK << ( i * ACTION_PERM_BITS ) ) ) );

	return( currentPerm );
	}

/* Update the action permissions for an object based on the composite
   permissions for it and a dependent object.  This is a special-case
   function because it has to operate with the object table unlocked.  This
   is necessary because the dependent object may be owned by another thread,
   and if we were to leave the object table locked the two would deadlock if
   we were sending the object a message while owning the object table at the
   same time that the other thread was sending a message while owning the 
   object.

   There is one (rather unlikely) potential race condition possible here in 
   which the object is destroyed and replaced by a new one while the object 
   table is unlocked, so we end up updating the action permissions for a 
   different object.  To protect against this, we check the unique ID after 
   we re-lock the object table to make sure that it's the same object */

static int updateDependentObjectPerms( const CRYPT_HANDLE objectHandle,
									   const CRYPT_HANDLE dependentObject )
	{
	const OBJECT_INFO *objectTable = krnlData->objectTable;
	const OBJECT_TYPE objectType = objectTable[ objectHandle ].type;
	const CRYPT_CONTEXT contextHandle = \
		( objectType == OBJECT_TYPE_CONTEXT ) ? objectHandle : dependentObject;
	const CRYPT_CERTIFICATE certHandle = \
		( objectType == OBJECT_TYPE_CERTIFICATE ) ? objectHandle : dependentObject;
	const unsigned int uniqueID = objectTable[ objectHandle ].uniqueID;
	int actionFlags = 0, status;
	ORIGINAL_INT_VAR( oldPerm, objectTable[ contextHandle ].actionFlags );

	/* Preconditions: Objects are valid, one is a cert and the other a
	   context, and they aren't dependent on each other (which would create
	   a dependency update loop).  Note that these checks aren't performed
	   at runtime since they've already been performed by the calling 
	   function, all we're doing here is establishing preconditions rather
	   than performing actual parameter checking */
	PRE( isValidObject( objectHandle ) );
	PRE( isValidHandle( dependentObject ) );
	PRE( ( objectTable[ objectHandle ].type == OBJECT_TYPE_CONTEXT && \
		   objectTable[ dependentObject ].type == OBJECT_TYPE_CERTIFICATE ) || \
		 ( objectTable[ objectHandle ].type == OBJECT_TYPE_CERTIFICATE && \
		   objectTable[ dependentObject ].type == OBJECT_TYPE_CONTEXT ) );
	PRE( objectTable[ objectHandle ].dependentObject != dependentObject || \
		 objectTable[ dependentObject ].dependentObject != objectHandle );

	/* Since we're about to send messages to the dependent object, we have to
	   unlock the object table */
	MUTEX_UNLOCK( objectTable );

	/* Make sure that we're not making a private key dependent on a cert, 
	   which is a public-key object.  We check this here rather than having
	   the caller check it because it requires having the object table
	   unlocked */
	if( objectType == OBJECT_TYPE_CERTIFICATE && \
		cryptStatusOK( \
			krnlSendMessage( dependentObject, IMESSAGE_CHECK, NULL,
							 MESSAGE_CHECK_PKC_PRIVATE ) ) )
		{
		assert( NOTREACHED );
		MUTEX_LOCK( objectTable );
		return( CRYPT_ARGERROR_OBJECT );
		}

	/* For each action type, enable its continued use only if the cert
	   allows it.  Because the certificate may not have been fully 
	   initialised yet (for example if we're attaching a context to a
	   cert that's in the process of being created), we have to perform 
	   a passive-container action-available check that also works on a 
	   low-state object rather than a standard active-object check.
	   
	   Because a key with a certificate attached indicates that it's 
	   (probably) being used for some function that involves interaction 
	   with a relying party (i.e. that it probably has more value than a raw
	   key with no strings attached), we set the action permission to
	   ACTION_PERM_NONE_EXTERNAL rather than allowing ACTION_PERM_ALL.  This
	   both ensures that it's only used in a safe manner via the cryptlib
	   internal mechanisms, and makes sure that it's not possible to utilize
	   the signature/encryption duality of some algorithms to create a
	   signature where it's been disallowed */
	if( cryptStatusOK( krnlSendMessage( certHandle,
				IMESSAGE_CHECK, NULL, MESSAGE_CHECK_PKC_SIGN_AVAIL ) ) )
		actionFlags |= \
			MK_ACTION_PERM( MESSAGE_CTX_SIGN, ACTION_PERM_NONE_EXTERNAL );
	if( cryptStatusOK( krnlSendMessage( certHandle,
				IMESSAGE_CHECK, NULL, MESSAGE_CHECK_PKC_SIGCHECK_AVAIL ) ) )
		actionFlags |= \
			MK_ACTION_PERM( MESSAGE_CTX_SIGCHECK, ACTION_PERM_NONE_EXTERNAL );
	if( cryptStatusOK( krnlSendMessage( certHandle,
				IMESSAGE_CHECK, NULL, MESSAGE_CHECK_PKC_ENCRYPT_AVAIL ) ) )
		actionFlags |= \
			MK_ACTION_PERM( MESSAGE_CTX_ENCRYPT, ACTION_PERM_NONE_EXTERNAL );
	if( cryptStatusOK( krnlSendMessage( certHandle,
				IMESSAGE_CHECK, NULL, MESSAGE_CHECK_PKC_DECRYPT_AVAIL ) ) )
		actionFlags |= \
			MK_ACTION_PERM( MESSAGE_CTX_DECRYPT, ACTION_PERM_NONE_EXTERNAL );
	if( cryptStatusOK( krnlSendMessage( certHandle,
				IMESSAGE_CHECK, NULL, MESSAGE_CHECK_PKC_KA_EXPORT_AVAIL ) ) )
		actionFlags |= \
			MK_ACTION_PERM( MESSAGE_CTX_ENCRYPT, ACTION_PERM_NONE_EXTERNAL );
	if( cryptStatusOK( krnlSendMessage( certHandle,
				IMESSAGE_CHECK, NULL, MESSAGE_CHECK_PKC_KA_IMPORT_AVAIL ) ) )
		actionFlags |= \
			MK_ACTION_PERM( MESSAGE_CTX_DECRYPT, ACTION_PERM_NONE_EXTERNAL );

	/* We're done querying the dependent object, re-lock the object table and
	   make sure that the original object hasn't been touched */
	MUTEX_LOCK( objectTable );
	if( objectTable[ objectHandle ].uniqueID != uniqueID )
		return( CRYPT_ERROR_SIGNALLED );
	status = setPropertyAttribute( contextHandle, CRYPT_IATTRIBUTE_ACTIONPERMS,
								   &actionFlags );

	/* Postcondition: The new permission is at least as restrictive (or more
	   so) than the old one */
	FORALL( i, 0, ACTION_PERM_COUNT,
			( objectTable[ contextHandle ].actionFlags & ( ACTION_PERM_MASK << ( i * 2 ) ) ) <= \
			( ORIGINAL_VALUE( oldPerm ) & ( ACTION_PERM_MASK << ( i * 2 ) ) ) );

	return( status );
	}

/****************************************************************************
*																			*
*							Init/Shutdown Functions							*
*																			*
****************************************************************************/

int initInternalMsgs( KERNEL_DATA *krnlDataPtr )
	{
	int i;

	/* Perform a consistency check on the object dependency ACL */
	for( i = 0; dependencyACLTbl[ i ].type != OBJECT_TYPE_NONE; i++ )
		{
		const DEPENDENCY_ACL *dependencyACL = &dependencyACLTbl[ i ];

		if( dependencyACL->type <= OBJECT_TYPE_NONE || \
			dependencyACL->type >= OBJECT_TYPE_LAST || \
			dependencyACL->dType <= OBJECT_TYPE_NONE || \
			dependencyACL->dType >= OBJECT_TYPE_LAST )
			return( CRYPT_ERROR_FAILED );
		if( ( dependencyACL->subTypeA & SUBTYPE_CLASS_B ) || \
			( dependencyACL->subTypeB & SUBTYPE_CLASS_A ) || \
			( dependencyACL->dSubTypeA & SUBTYPE_CLASS_B ) || \
			( dependencyACL->dSubTypeB & SUBTYPE_CLASS_A ) )
			return( CRYPT_ERROR_FAILED );
		}

	/* Set up the reference to the kernel data block */
	krnlData = krnlDataPtr;

	return( CRYPT_OK );
	}

void endInternalMsgs( void )
	{
	krnlData = NULL;
	}

/****************************************************************************
*																			*
*							Get/Set Property Attributes						*
*																			*
****************************************************************************/

/* Get/set object property attributes.  We differentiate between a small
   number of user-accessible properties such as the object's owner, and
   properties that are only accessible by cryptlib.  The user-accessible
   properties can be locked, which makes them immutable (at least to being
   explicitly set, they can still be implicitly altered, for example setting
   a new object owner decrements the forwardcount value) and also unreadable
   by the user */

int getPropertyAttribute( const int objectHandle,
						  const CRYPT_ATTRIBUTE_TYPE attribute,
						  void *messageDataPtr )
	{
	const OBJECT_INFO *objectInfoPtr = &krnlData->objectTable[ objectHandle ];
	int *valuePtr = ( int * ) messageDataPtr;

	/* Preconditions */
	PRE( isValidObject( objectHandle ) );
	PRE( attribute == CRYPT_PROPERTY_OWNER || \
		 attribute == CRYPT_PROPERTY_FORWARDCOUNT || \
		 attribute == CRYPT_PROPERTY_LOCKED || \
		 attribute == CRYPT_PROPERTY_USAGECOUNT || \
		 attribute == CRYPT_IATTRIBUTE_TYPE || \
		 attribute == CRYPT_IATTRIBUTE_SUBTYPE || \
		 attribute == CRYPT_IATTRIBUTE_STATUS || \
		 attribute == CRYPT_IATTRIBUTE_INTERNAL || \
		 attribute == CRYPT_IATTRIBUTE_ACTIONPERMS );
	PRE( isWritePtr( messageDataPtr, sizeof( int ) ) );

	switch( attribute )
		{
		/* User-accessible properties */
		case CRYPT_PROPERTY_OWNER:
			/* We allow this to be read since its value can be determined
			   anyway with a trial access */
			if( !( objectInfoPtr->flags & OBJECT_FLAG_OWNED ) )
				return( CRYPT_ERROR_NOTINITED );
#ifdef USE_THREADS
  #ifdef NONSCALAR_THREADS
			/* A very small number of pthreads implementations use non-
			   scalar thread IDs, which we can't easily handle when all we
			   have is an integer handle.  However, the need to bind threads
			   to objects only exists because of Win32 security holes
			   arising from the ability to perform thread injection, so this
			   isn't a big issue */
			return( CRYPT_ERROR_FAILED );
  #else
			*valuePtr = ( int ) objectInfoPtr->objectOwner;
  #endif /* Non-scalar threading environments */
#else
			*valuePtr = 0;
#endif /* USE_THREADS */
			break;

		case CRYPT_PROPERTY_FORWARDCOUNT:
			if( objectInfoPtr->flags & OBJECT_FLAG_ATTRLOCKED )
				return( CRYPT_ERROR_PERMISSION );
			*valuePtr = objectInfoPtr->forwardCount;
			break;

		case CRYPT_PROPERTY_LOCKED:
			/* We allow this to be read since its value can be determined
			   anyway with a trial write */
			*( ( BOOLEAN * ) messageDataPtr ) = \
						( objectInfoPtr->flags & OBJECT_FLAG_ATTRLOCKED ) ? \
						TRUE : FALSE;
			break;

		case CRYPT_PROPERTY_USAGECOUNT:
			*valuePtr = objectInfoPtr->usageCount;
			break;

		/* Internal properties */
		case CRYPT_IATTRIBUTE_TYPE :
			*valuePtr = objectInfoPtr->type;
			break;

		case CRYPT_IATTRIBUTE_SUBTYPE :
			*valuePtr = objectInfoPtr->subType;
			break;

		case CRYPT_IATTRIBUTE_STATUS:
			*valuePtr = objectInfoPtr->flags & OBJECT_FLAGMASK_STATUS;
			break;

		case CRYPT_IATTRIBUTE_INTERNAL:
			*( ( BOOLEAN * ) messageDataPtr ) = \
					( objectInfoPtr->flags & OBJECT_FLAG_INTERNAL ) ? \
					TRUE : FALSE;
			break;

		case CRYPT_IATTRIBUTE_ACTIONPERMS:
			*valuePtr = objectInfoPtr->actionFlags;
			break;

		default:
			assert( NOTREACHED );
			return( CRYPT_ARGERROR_VALUE );
		}

	return( CRYPT_OK );
	}

int setPropertyAttribute( const int objectHandle,
						  const CRYPT_ATTRIBUTE_TYPE attribute,
						  void *messageDataPtr )
	{
	OBJECT_INFO *objectInfoPtr = &krnlData->objectTable[ objectHandle ];
	const int value = *( ( int * ) messageDataPtr );

	/* Preconditions */
	PRE( isValidObject( objectHandle ) );
	PRE( attribute == CRYPT_PROPERTY_HIGHSECURITY || \
		 attribute == CRYPT_PROPERTY_OWNER || \
		 attribute == CRYPT_PROPERTY_FORWARDCOUNT || \
		 attribute == CRYPT_PROPERTY_LOCKED || \
		 attribute == CRYPT_PROPERTY_USAGECOUNT || \
		 attribute == CRYPT_IATTRIBUTE_STATUS || \
		 attribute == CRYPT_IATTRIBUTE_INTERNAL || \
		 attribute == CRYPT_IATTRIBUTE_ACTIONPERMS || \
		 attribute == CRYPT_IATTRIBUTE_LOCKED );
	PRE( isReadPtr( messageDataPtr, sizeof( int ) ) );
	PRE( objectHandle >= NO_SYSTEM_OBJECTS || \
		 attribute == CRYPT_IATTRIBUTE_STATUS );

	switch( attribute )
		{
		/* User-accessible properties */
		case CRYPT_PROPERTY_HIGHSECURITY:
			/* This is a combination property that makes an object owned,
			   non-forwardable, and locked */
			if( objectInfoPtr->flags & OBJECT_FLAG_ATTRLOCKED )
				return( CRYPT_ERROR_PERMISSION );
#ifdef USE_THREADS
			objectInfoPtr->objectOwner = THREAD_SELF();
#endif /* USE_THREADS */
			objectInfoPtr->forwardCount = 0;
			objectInfoPtr->flags |= OBJECT_FLAG_ATTRLOCKED | OBJECT_FLAG_OWNED;
			break;

		case CRYPT_PROPERTY_OWNER:
			/* This property can still be changed (even if the object is
			   locked) until the forwarding count drops to zero, otherwise
			   locking the object would prevent any forwarding */
			if( objectInfoPtr->forwardCount != CRYPT_UNUSED )
				{
				if( objectInfoPtr->forwardCount <= 0 )
					return( CRYPT_ERROR_PERMISSION );
				objectInfoPtr->forwardCount--;
				}
			if( value == CRYPT_UNUSED )
				objectInfoPtr->flags &= ~OBJECT_FLAG_OWNED;
			else
				{
#if defined( USE_THREADS ) && !defined( NONSCALAR_THREADS )
				objectInfoPtr->objectOwner = ( THREAD_HANDLE ) value;
				objectInfoPtr->flags |= OBJECT_FLAG_OWNED;
#endif /* USE_THREADS && scalar threading environments */
				}
			break;

		case CRYPT_PROPERTY_FORWARDCOUNT:
			if( objectInfoPtr->flags & OBJECT_FLAG_ATTRLOCKED )
				return( CRYPT_ERROR_PERMISSION );
			if( objectInfoPtr->forwardCount != CRYPT_UNUSED && \
				objectInfoPtr->forwardCount < value )
				/* Once set the forward count can only be decreased, never
				   increased */
				return( CRYPT_ERROR_PERMISSION );
			objectInfoPtr->forwardCount = value;
			break;

		case CRYPT_PROPERTY_LOCKED:
			/* Precondition: This property can only be set to true */
			PRE( value );

			objectInfoPtr->flags |= OBJECT_FLAG_ATTRLOCKED;
			break;

		case CRYPT_PROPERTY_USAGECOUNT:
			if( objectInfoPtr->flags & OBJECT_FLAG_ATTRLOCKED || \
				( objectInfoPtr->usageCount != CRYPT_UNUSED && \
				  objectInfoPtr->usageCount < value ) )
				/* Once set the usage count can only be decreased, never
				   increased */
				return( CRYPT_ERROR_PERMISSION );
			objectInfoPtr->usageCount = value;
			break;

		/* Internal properties */
		case CRYPT_IATTRIBUTE_STATUS:
			/* We're clearing an error/abnormal state or setting the object
			   to the busy state */
			PRE( value == CRYPT_OK || value == CRYPT_ERROR_TIMEOUT );

			if( isInvalidObjectState( objectHandle ) )
				{
				/* If the object is in an abnormal state, we can only (try to)
				   return it back to the normal state after the problem is
				   resolved */
				PRE( value == CRYPT_OK );

				/* If we're resetting the object status from busy to OK,
				   notify the object in case there's any extra processing to
				   be done */
				if( objectInfoPtr->flags & OBJECT_FLAG_BUSY )
					{
					/* Precondition: Only contexts can be busy */
					PRE( objectInfoPtr->type == OBJECT_TYPE_CONTEXT );

					/* If the notification returns an error, the object is
					   still performing some sort of processing (e.g. cleanup/
					   shutdown), don't reset the status (it'll be done later
					   when the object is ready) */
					if( objectInfoPtr->messageFunction( objectInfoPtr->objectPtr,
									MESSAGE_CHANGENOTIFY, messageDataPtr,
									MESSAGE_CHANGENOTIFY_STATUS ) == CRYPT_OK )
						objectInfoPtr->flags &= ~OBJECT_FLAG_BUSY;
					break;
					}

				/* If we're processing a notification from the caller that
				   the object init is complete and the object was destroyed
				   while it was being created (which sets its state to
				   CRYPT_ERROR_SIGNALLED), tell the caller to convert the
				   message to a destroy object message unless it's a system
				   object, which can't be explicitly destroyed.  In this case
				   we just return an error so the cryptlib init fails */
				if( objectInfoPtr->flags & OBJECT_FLAG_SIGNALLED )
					return( ( objectHandle < NO_SYSTEM_OBJECTS ) ?
							CRYPT_ERROR_SIGNALLED : OK_SPECIAL );

				/* We're transitioning the object to the initialised state */
				PRE( objectInfoPtr->flags & OBJECT_FLAG_NOTINITED );
				objectInfoPtr->flags &= ~OBJECT_FLAG_NOTINITED;
				POST( !( objectInfoPtr->flags & OBJECT_FLAG_NOTINITED ) );
				break;
				}

			/* Inner precondition: The object is in a valid state */
			PRE( !isInvalidObjectState( objectHandle ) );

			/* We're setting the object's busy flag because it's about to
			   perform an async op */
			if( value == CRYPT_ERROR_TIMEOUT )
				objectInfoPtr->flags |= OBJECT_FLAG_BUSY;
			break;

		case CRYPT_IATTRIBUTE_INTERNAL:
			if( value )
				{
				PRE( !( objectInfoPtr->flags & OBJECT_FLAG_INTERNAL ) );

				objectInfoPtr->flags |= OBJECT_FLAG_INTERNAL;
				}
			else
				{
				PRE( objectInfoPtr->flags & OBJECT_FLAG_INTERNAL );

				objectInfoPtr->flags &= ~OBJECT_FLAG_INTERNAL;
				}
			break;

		case CRYPT_IATTRIBUTE_ACTIONPERMS:
			objectInfoPtr->actionFlags = \
					updateActionPerms( objectInfoPtr->actionFlags, value );
			break;

		case CRYPT_IATTRIBUTE_LOCKED:
			/* Incremement or decrement the object's lock count depending on
			   whether we're locking or unlocking it */
			if( value )
				{
				objectInfoPtr->lockCount++;
#ifdef USE_THREADS
				objectInfoPtr->lockOwner = THREAD_SELF();
#endif /* USE_THREADS */
				}
			else
				{
				/* Precondition: The lock count is positive */
				PRE( objectInfoPtr->lockCount > 0 );

				if( objectInfoPtr->lockCount <= 0 )
					{
					assert( NOTREACHED );
					return( CRYPT_ERROR_PERMISSION );
					}
				objectInfoPtr->lockCount--;
				}

			/* If it's a certificate, notify it that it should save/restore 
			   its internal state */
			if( objectInfoPtr->type == OBJECT_TYPE_CERTIFICATE )
				objectInfoPtr->messageFunction( objectInfoPtr->objectPtr,
									MESSAGE_CHANGENOTIFY, messageDataPtr,
									MESSAGE_CHANGENOTIFY_STATE );
			break;

		default:
			assert( NOTREACHED );
			return( CRYPT_ARGERROR_VALUE );
		}

	return( CRYPT_OK );
	}

/****************************************************************************
*																			*
*							Update Internal Properties						*
*																			*
****************************************************************************/

/* Increment/decrement the reference count for an object.  This adjusts the
   reference count as appropriate and sends destroy messages if the reference
   count goes negative */

int incRefCount( const int objectHandle, const int dummy1,
				 const void *dummy2, const BOOLEAN dummy3 )
	{
	OBJECT_INFO *objectTable = krnlData->objectTable;
	ORIGINAL_INT_VAR( refCt, objectTable[ objectHandle ].referenceCount );

	/* Preconditions */
	PRE( isValidObject( objectHandle ) );
	POST( objectTable[ objectHandle ].referenceCount >= 0 );

	/* Increment an object's reference count */
	objectTable[ objectHandle ].referenceCount++;

	/* Postcondition: We incremented the reference count and it's now greater
	   than zero (the ground state) */
	POST( objectTable[ objectHandle ].referenceCount >= 1 );
	POST( objectTable[ objectHandle ].referenceCount == \
		  ORIGINAL_VALUE( refCt ) + 1 );

	return( CRYPT_OK );
	}

int decRefCount( const int objectHandle, const int dummy1,
				 const void *dummy2, const BOOLEAN isInternal )
	{
	OBJECT_INFO *objectTable = krnlData->objectTable;
	int status;
	ORIGINAL_INT_VAR( refCt, objectTable[ objectHandle ].referenceCount );

	/* Preconditions */
	PRE( isValidObject( objectHandle ) );

	/* If the message is coming from an external source (in other words if
	   it's an external caller destroying the object), make the object 
	   internal.  This marks it as invalid for any external access, so that
	   to the caller it looks like it's been destroyed even if its reference 
	   count keeps it active */
	if( !isInternal )
		{
		PRE( !isInternalObject( objectHandle ) );

		objectTable[ objectHandle ].flags |= OBJECT_FLAG_INTERNAL;

		POST( isInternalObject( objectHandle ) );
		}

	/* Decrement an object's reference count */
	if( objectTable[ objectHandle ].referenceCount > 0 )
		{
		objectTable[ objectHandle ].referenceCount--;

		/* Postconditions: We decremented the reference count and it's
		   greater than or equal to zero (the ground state) */
		POST( objectTable[ objectHandle ].referenceCount >= 0 );
		POST( objectTable[ objectHandle ].referenceCount == \
			  ORIGINAL_VALUE( refCt ) - 1 );

		return( CRYPT_OK );
		}

	/* We're already at a single reference, destroy the object.  Since this
	   can entail arbitrary amounts of processing during the object shutdown
	   phase, we have to unlock the object table around the call */
	MUTEX_UNLOCK( objectTable );
	status = krnlSendNotifier( objectHandle, IMESSAGE_DESTROY );
	MUTEX_LOCK( objectTable );

	return( status );
	}

/* Get/set dependent objects for an object */

int getDependentObject( const int objectHandle, const int targetType,
						const void *messageDataPtr, const BOOLEAN dummy )
	{
	int *valuePtr = ( int * ) messageDataPtr, localObjectHandle;

	/* Preconditions */
	PRE( isValidObject( objectHandle ) );
	PRE( isValidType( targetType ) );
	PRE( messageDataPtr != NULL );

	/* Clear return value */
	*valuePtr = CRYPT_ERROR;

	localObjectHandle = findTargetType( objectHandle, targetType );
	if( cryptStatusError( localObjectHandle ) )
		{
		/* Postconditions: No dependent object found */
		POST( *valuePtr == CRYPT_ERROR );

		return( CRYPT_ARGERROR_OBJECT );
		}
	*valuePtr = localObjectHandle;

	/* Postconditions: We found a dependent object */
	POST( isValidObject( *valuePtr ) && \
		  isSameOwningObject( objectHandle, *valuePtr ) );

	return( CRYPT_OK );
	}

int setDependentObject( const int objectHandle, const int incReferenceCount,
						const void *messageDataPtr, const BOOLEAN dummy )
	{
	OBJECT_INFO *objectTable = krnlData->objectTable;
	OBJECT_INFO *objectInfoPtr = &objectTable[ objectHandle ];
	const OBJECT_INFO *dependentObjectInfoPtr = &objectTable[ objectHandle ];
	const int dependentObject = *( ( int * ) messageDataPtr );
	const DEPENDENCY_ACL *dependencyACL = NULL;
	int *objectHandlePtr, i, status;

	/* Preconditions: Parameters are valid */
	PRE( isValidObject( objectHandle ) );
	PRE( incReferenceCount == TRUE || incReferenceCount == FALSE );
	PRE( isValidHandle( dependentObject ) );

	/* Make sure that the object is valid, it may have been signalled after 
	   the message was sent */
	if( !isValidObject( dependentObject ) )
		return( CRYPT_ERROR_SIGNALLED );
	dependentObjectInfoPtr = &objectTable[ dependentObject ];
	objectHandlePtr = ( dependentObjectInfoPtr->type == OBJECT_TYPE_DEVICE ) ? \
			&objectInfoPtr->dependentDevice : &objectInfoPtr->dependentObject;

	/* Basic validity checks: There can't already be a dependent object set */
	if( *objectHandlePtr != CRYPT_ERROR )
		{
		/* There's already a dependent object present and we're trying to
		   overwrite it with a new one, something is seriously wrong */
		assert( NOTREACHED );
		return( CRYPT_ARGERROR_VALUE );
		}
	
	/* More complex validity checks to ensure that the object table is 
	   consistent: The object isn't already dependent on the dependent object 
	   (making the dependent object then dependent on the object would 
	   create a loop), and the object won't be dependent on its own object 
	   type unless it's a device dependent on the system device */
	if( ( ( ( objectInfoPtr->type == OBJECT_TYPE_DEVICE ) ? \
			  dependentObjectInfoPtr->dependentDevice : \
			  dependentObjectInfoPtr->dependentObject ) == objectHandle ) || \
		( objectInfoPtr->type == dependentObjectInfoPtr->type && \
		  dependentObject != SYSTEM_OBJECT_HANDLE ) )
		{
		assert( NOTREACHED );
		return( CRYPT_ARGERROR_VALUE );
		}

	/* Find the dependency ACL entry for this object/dependent object 
	   combination.  Since there can be more than one dependent object
	   type for an object, we check subtypes as well */
	for( i = 0; dependencyACLTbl[ i ].type != OBJECT_TYPE_NONE; i++ )
		if( dependencyACLTbl[ i ].type == objectInfoPtr->type && \
			dependencyACLTbl[ i ].dType == dependentObjectInfoPtr->type && \
			( isValidSubtype( dependencyACLTbl[ i ].dSubTypeA, \
							  dependentObjectInfoPtr->subType ) || \
			  isValidSubtype( dependencyACLTbl[ i ].dSubTypeB, \
							  dependentObjectInfoPtr->subType ) ) )
			{
			dependencyACL = &dependencyACLTbl[ i ];
			break;
			}
	if( dependencyACL == NULL )
		{
		assert( NOTREACHED );
		return( CRYPT_ARGERROR_VALUE );
		}

	/* Inner precondition: We have the appropriate ACL for this combination 
	   of object and dependent object */
	PRE( dependencyACL->type == objectInfoPtr->type && \
		 dependencyACL->dType == dependentObjectInfoPtr->type && \
		 ( isValidSubtype( dependencyACL->dSubTypeA, \
						   dependentObjectInfoPtr->subType ) || \
		   isValidSubtype( dependencyACL->dSubTypeB, \
						   dependentObjectInfoPtr->subType ) ) );

	/* Type-specific checks.  For PKC context -> cert and cert -> PKC context
	   attaches we should also check that the primary PKC object is a 
	   private-key object and the dependent PKC object is a public-key object
	   to catch things like a private key depending on a (public-key) cert,
	   however this requires unlocking the object table in order to send the
	   context a check message.  Since this requires additional precautions,
	   we leave it for updateDependentObjectPerms(), which has to unlock the
	   table for its own update operations */
	if( !isValidSubtype( dependencyACL->subTypeA, \
						 objectInfoPtr->subType ) && \
		!isValidSubtype( dependencyACL->subTypeB, \
						 objectInfoPtr->subType ) )
		{
		assert( NOTREACHED );
		return( CRYPT_ARGERROR_VALUE );
		}
	if( !isValidSubtype( dependencyACL->dSubTypeA, \
						 dependentObjectInfoPtr->subType ) && \
		!isValidSubtype( dependencyACL->dSubTypeB, \
						 dependentObjectInfoPtr->subType ) )
		{
		assert( NOTREACHED );
		return( CRYPT_ARGERROR_VALUE );
		}

	/* Inner precondition */
	PRE( *objectHandlePtr == CRYPT_ERROR );
	PRE( isSameOwningObject( objectHandle, dependentObject ) );

	/* Certs and contexts have special relationships in that the cert can
	   constrain the use of the context beyond its normal level.  If we're
	   performing this type of object attachment, we have to adjust one
	   object's behaviour based on the permissions of the other one.  We do
	   this before we increment the reference count because the latter can 
	   never fail so we don't have to worry about undoing the update */
	if( dependencyACL->flags & DEP_FLAG_UPDATEDEP )
		{
		status = updateDependentObjectPerms( objectHandle, dependentObject );
		if( cryptStatusError( status ) )
			return( status );
		}

	/* Update the dependent object's reference count if required and record
	   the new status in the object table.  Dependent objects can be
	   established in one of two ways, by taking an existing object and
	   attaching it to another object (which increments its reference count,
	   since it's now being referred to by the original owner and by the
	   object it's  attached to), or by creating a new object and attaching
	   it to another object (which doesn't increment the reference count
	   since it's only referred to by the controlling object).  An example of
	   the former operation is adding a context from a cert request to a cert
	   (the cert request is referenced by both the caller and the cert), an
	   example of the latter operation is attaching a data-only cert to a
	   context (the cert is only referenced by the context) */
	if( incReferenceCount )
		{
		status = incRefCount( dependentObject, 0, NULL, TRUE );
		if( cryptStatusError( status ) )
			return( status );
		}
	*objectHandlePtr = dependentObject;

	/* Postconditions */
	POST( isValidObject( *objectHandlePtr ) && \
		  isSameOwningObject( objectHandle, *objectHandlePtr ) );

	return( CRYPT_OK );
	}

/* Clone an object.  The older copy-on-write implementation didn't actually 
   do anything at this point except check that the access was valid and set 
   the aliased and cloned flags to indicate that the object needed to be 
   handled specially if a write access was made to it, but with the kernel
   tracking instance data we can do a copy immediately to create two 
   distinct objects */

int cloneObject( const int objectHandle, const int clonedObject,
				 const void *dummy1, const BOOLEAN dummy2 )
	{
	OBJECT_INFO *objectInfoPtr = &krnlData->objectTable[ objectHandle ];
	OBJECT_INFO *clonedObjectInfoPtr = &krnlData->objectTable[ clonedObject ];
	int actionFlags, status;

	/* Preconditions */
	PRE( isValidObject( objectHandle ) && \
		 objectHandle >= NO_SYSTEM_OBJECTS );
	PRE( !isClonedObject( objectHandle ) && \
		 !isAliasedObject( objectHandle ) );
	PRE( objectInfoPtr->type == OBJECT_TYPE_CONTEXT );
	PRE( isValidObject( clonedObject ) && \
		 clonedObject >= NO_SYSTEM_OBJECTS );
	PRE( !isClonedObject( clonedObject ) && \
		 !isAliasedObject( clonedObject ) );
	PRE( clonedObjectInfoPtr->type == OBJECT_TYPE_CONTEXT );
	PRE( objectHandle != clonedObject );

	/* Make sure that the original object is in the high state.  This will
	   have been checked by the caller anyway, but we check again here to
	   make sure */
	if( !isInHighState( objectHandle ) )
		return( CRYPT_ERROR_NOTINITED );

	/* Cloning of non-native contexts is somewhat complex because we usually
	   can't clone a device object, so we have to detect requests to clone
	   these objects and increment their reference count instead.  This
	   isn't a major problem because cryptlib always creates native contexts
	   for clonable algorithms, if the user explicitly overrides this by
	   using their own device-specific context then the usage will usually
	   be create, add to envelope, destroy, so there's no need to clone the
	   context anyway.  The only that time there's a potential problem is if
	   they override the use of native contexts by adding device contexts to
	   multiple envelopes, but in that case it's assumed that they'll be
	   aware of potential problems with this approach */
	if( objectInfoPtr->dependentDevice != SYSTEM_OBJECT_HANDLE )
		return( incRefCount( objectHandle, 0, NULL, TRUE ) );

	/* Since this is an internal-use-only object, lock down the action
	   permissions so that only encryption and hash actions from internal
	   sources are allowed (assuming they were allowed to begin with).
	   Keygen is disabled entirely (there should already be a key loaded),
	   and signing isn't possible with a non-PKC object anyway.  This takes
	   advantage of the ratchet enforced for the action permissions, which
	   can only make them more restrictive than the existing permissions, to 
	   avoid having to read and modify each permission individually */
	actionFlags = \
		MK_ACTION_PERM( MESSAGE_CTX_ENCRYPT, ACTION_PERM_NONE_EXTERNAL ) | \
		MK_ACTION_PERM( MESSAGE_CTX_DECRYPT, ACTION_PERM_NONE_EXTERNAL ) | \
		MK_ACTION_PERM( MESSAGE_CTX_HASH, ACTION_PERM_NONE_EXTERNAL );
	status = setPropertyAttribute( clonedObject, CRYPT_IATTRIBUTE_ACTIONPERMS,
								   &actionFlags );
	if( cryptStatusError( status ) )
		return( status );

	/* Postcondition: The cloned object can only be used internally */
	POST( ( clonedObjectInfoPtr->actionFlags & ~ACTION_PERM_NONE_EXTERNAL_ALL ) == 0 );

	/* Inner precondition: The instance data is valid and ready to be 
	   copied */
	PRE( isWritePtr( objectInfoPtr->objectPtr, objectInfoPtr->objectSize ) );
	PRE( isWritePtr( clonedObjectInfoPtr->objectPtr, 
					 clonedObjectInfoPtr->objectSize ) );
	PRE( objectInfoPtr->objectSize == clonedObjectInfoPtr->objectSize );

#if 0	/* 18/2/04 No need for copy-on-write any more since we can just copy 
				   across the instance data referenced in the object table */
	/* Mark the two objects as being aliases, and the (incomplete) clone as
	   a cloned object */
	objectInfoPtr->flags |= OBJECT_FLAG_ALIASED;
	objectInfoPtr->clonedObject = clonedObject;
	clonedObjectInfoPtr->flags |= OBJECT_FLAG_ALIASED | OBJECT_FLAG_CLONE;
	clonedObjectInfoPtr->clonedObject = objectHandle;

	/* Postconditions: The objects are marked as aliased objects and the
	   cloned object as a clone */
	POST( isAliasedObject( objectHandle ) && !isClonedObject( objectHandle ) );
	POST( isAliasedObject( clonedObject ) && isClonedObject( clonedObject ) );
	POST( !isClonedObject( clonedObjectInfoPtr->clonedObject ) );
	POST( isClonedObject( objectInfoPtr->clonedObject ) );
	POST( objectHandle != clonedObject );
#else
	/* Copy across the object contents and reset any instance-specific 
	   information.  We only update the owning object if required, in
	   almost all cases this will be the system device so there's no need
	   to perform the update */
	memcpy( clonedObjectInfoPtr->objectPtr, objectInfoPtr->objectPtr,
			objectInfoPtr->objectSize );
	objectInfoPtr->messageFunction( clonedObjectInfoPtr->objectPtr,
									MESSAGE_CHANGENOTIFY, 
									( void * ) &clonedObject,
									MESSAGE_CHANGENOTIFY_OBJHANDLE );
	if( objectInfoPtr->owner != clonedObjectInfoPtr->owner )
		objectInfoPtr->messageFunction( clonedObjectInfoPtr->objectPtr,
										MESSAGE_CHANGENOTIFY, 
										&clonedObjectInfoPtr->owner,
										MESSAGE_CHANGENOTIFY_OWNERHANDLE );

	/* We've copied across the object's state, the cloned object is now
	   initialised ready for use */
	clonedObjectInfoPtr->flags |= OBJECT_FLAG_HIGH;
#endif /* 0 */

	return( CRYPT_OK );
	}
