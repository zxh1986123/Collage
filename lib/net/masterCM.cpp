
/* Copyright (c) 2010, Stefan Eilemann <eile@equalizergraphics.com> 
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "masterCM.h"

#include "command.h"
#include "commands.h"
#include "log.h"
#include "object.h"
#include "objectSlaveDataIStream.h"
#include "packets.h"

namespace eq
{
namespace net
{
typedef CommandFunc<MasterCM> CmdFunc;

MasterCM::MasterCM( Object* object )
        : _object( object )
        , _version( VERSION_NONE )
{
    // sync commands are send to all instances, even the master gets it
    registerCommand( CMD_OBJECT_INSTANCE,
                     CmdFunc( this, &MasterCM::_cmdDiscard ), 0 );
    registerCommand( CMD_OBJECT_SLAVE_DELTA,
                     CmdFunc( this, &MasterCM::_cmdSlaveDelta ), 0 );
}

MasterCM::~MasterCM()
{
    EQASSERTINFO( _pendingDeltas.empty(), "Incomplete slave commits pending" );
    EQASSERTINFO( _queuedDeltas.isEmpty(), "Unapplied slave commits" );

    while( !_pendingDeltas.empty( ))
    {
        delete _pendingDeltas.back().second;
        _pendingDeltas.pop_back();
    }

    ObjectDataIStream* is = 0;
    while( _queuedDeltas.tryPop( is ))
        delete is;

    if( !_slaves.empty( ))
        EQWARN << _slaves.size() 
               << " slave nodes subscribed during deregisterObject of "
               << typeid( *_object ).name() << std::endl;
    _slaves.clear();
}

uint32_t MasterCM::commitNB()
{
    NodePtr localNode = _object->getLocalNode();
    ObjectCommitPacket packet;
    packet.instanceID = _object->_instanceID;
    packet.requestID  = localNode->registerRequest();

    _object->send( localNode, packet );
    return packet.requestID;
}

uint32_t MasterCM::commitSync( const uint32_t commitID )
{
    NodePtr localNode = _object->getLocalNode();
    uint32_t version = VERSION_NONE;
    localNode->waitRequest( commitID, version );
    return version;
}

uint32_t MasterCM::sync( const uint32_t version )
{
    EQASSERT( version == VERSION_NEXT || version == VERSION_HEAD );
    EQLOG( LOG_OBJECTS ) << "sync to v" << version << ", id " 
                         << _object->getID() << "." << _object->getInstanceID()
                         << std::endl;

    if( version == VERSION_NEXT )
    {
        ObjectDataIStream* is = _queuedDeltas.pop();
        _object->unpack( *is );
        EQASSERTINFO( is->getRemainingBufferSize() == 0 && 
                      is->nRemainingBuffers()==0,
                      "Object " << typeid( *_object ).name() <<
                      " did not unpack all data" );
        delete is;
        return _version;
    }
    // else

    ObjectDataIStream* is = 0;
    while( _queuedDeltas.tryPop( is ))
    {
        EQASSERT( is );
        _object->unpack( *is );
        EQASSERTINFO( is->getRemainingBufferSize() == 0 && 
                      is->nRemainingBuffers()==0,
                      "Object " << typeid( *_object ).name() <<
                      " did not unpack all data" );
        delete is;
    }
    return _version;
}

void MasterCM::addOldMaster( NodePtr node, const uint32_t instanceID )
{
    EQASSERT( _version != VERSION_NONE );

    // add to subscribers
    ++_slavesCount[ node->getNodeID() ];
    _slaves.push_back( node );
    stde::usort( _slaves );

    ObjectVersionPacket packet;
    packet.instanceID = instanceID;
    packet.version    = _version;
    _object->send( node, packet );
}

//---------------------------------------------------------------------------
// command handlers
//---------------------------------------------------------------------------
CommandResult MasterCM::_cmdSlaveDelta( Command& command )
{
    CHECK_THREAD( _cmdThread );
    const ObjectSlaveDeltaPacket* packet = 
        command.getPacket< ObjectSlaveDeltaPacket >();

    EQASSERTINFO( _pendingDeltas.size() < 100,
                  "More than 100 unfinished slave commits!?" );

    ObjectDataIStream* istream = 0;
    PendingStreams::iterator i = _pendingDeltas.begin();
    for( ; i != _pendingDeltas.end(); ++i )
    {
        PendingStream& pending = *i;
        if( pending.first == packet->commit )
        {
            istream = pending.second;
            break;
        }
    }

    if( !istream )
    {
        EQASSERT( i == _pendingDeltas.end( ));
        istream = new ObjectSlaveDeltaDataIStream;
    }

    istream->addDataPacket( command );

    if( istream->isReady( ))
    {
        if( i != _pendingDeltas.end( ))
            _pendingDeltas.erase( i );

        _queuedDeltas.push( istream );
        _object->notifyNewVersion();
        EQLOG( LOG_OBJECTS )
            << "Queued slave commit " << packet->commit << " object "
            << _object->getID() << " " << typeid( *_object ).name()
            << std::endl;
    }
    else if( i == _pendingDeltas.end( ))
    {
        _pendingDeltas.push_back( PendingStream( packet->commit, istream ));
        EQLOG( LOG_OBJECTS )
            << "New incomplete slave commit " << packet->commit << " object "
            << _object->getID() << " " << typeid( *_object ).name() <<std::endl;
    }
    else
        EQLOG( LOG_OBJECTS )
            << "Got data for incomplete slave commit " << packet->commit
            << " object " << _object->getID() << " "
            << typeid( *_object ).name() << std::endl;

    return COMMAND_HANDLED;
}

}
}