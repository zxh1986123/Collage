
/* Copyright (c) 2005, Stefan Eilemann <eile@equalizergraphics.com> 
   All rights reserved. */

#ifndef EQNET_SESSION_H
#define EQNET_SESSION_H

#include "base.h"
#include "commands.h"
#include "global.h"
#include "idHash.h"
#include "node.h"
#include "object.h"

#include <eq/base/base.h>
#include <eq/base/idPool.h>
#include <eq/base/requestHandler.h>

namespace eqNet
{
    /**
     * Manages a session.
     *
     * A session provides unique identifiers for a number of nodes.
     */
    class Session : public Base
    {
    public:
        /** 
         * Constructs a new session.
         *
         * @param nCommands the highest command ID to be handled by the node, at
         *                  least <code>CMD_SESSION_CUSTOM</code>.
         * @param threadSafe if <code>true</code>, all public functions are
         *                   thread-safe.
         */
        Session( const uint32_t nCommands = CMD_SESSION_CUSTOM, 
                 const bool threadSafe = false );

        /** 
         * Returns the name of the session.
         * 
         * @return the name of the session.
         */
        const std::string& getName() const { return _name; }

        /** 
         * Returns the identifier of this session.
         * 
         * @return the identifier.
         */
        uint32_t getID() const { return _id; }

        /** 
         * @return the local node holding this session. 
         */
        Node* getNode(){ return _localNode.get(); }

//         /** 
//          * @return the server hosting this session. 
//          */
//         Node* getServer(){ return _server.get(); }

        /** 
         * Dispatches a command packet to the appropriate object.
         * 
         * @param node the node which sent the packet.
         * @param packet the packet.
         * @return the result of the operation.
         * @sa handleCommand
         */
        CommandResult dispatchPacket( Node* node, const Packet* packet );

        /**
         * @name Identifier management
         */
        //*{
        /** 
         * Generates a continous block of unique identifiers.
         * 
         * @param range the size of the block.
         * @return the first identifier of the block, or <code>0</code> if no
         *         identifier is available.
         * @todo getID( TYPE_OBJECT | ... | TYPE_CUSTOM );
         */
        uint32_t genIDs( const uint32_t range );

        /** 
         * Frees a continous block of unique identifiers.
         * 
         * @param start the first identifier in the block.
         * @param range the size of the block.
         */
        void freeIDs( const uint32_t start, const uint32_t range );

        /** 
         * Set the master node for a block of identifiers.
         * 
         * This can be used to identify the node which is responsible for the
         * object, action or information associated with an identifier. The
         * identifiers must be unique, it is therefore advised to allocate them
         * using genIDs().
         *
         * The master node must be reachable from this node and known by the
         * session server node.
         *
         * @param start the first identifier of the block.
         * @param range the size of the block.
         * @param master the master node for the block of identifiers.
         */
        void setIDMaster( const uint32_t start, const uint32_t range, 
                          Node* master );

        /** 
         * Delete the master node for a block of identifiers.
         * 
         * @param start the first identifier of the block.
         * @param range the size of the block.
         */
        void unsetIDMaster( const uint32_t start, const uint32_t range );

        /** 
         * Returns the master node for an identifier.
         * 
         * @param id the identifier.
         * @return the master node, or an invalid RefPtr if no master node is
         *         set for the identifier.
         */
        eqBase::RefPtr<Node> getIDMaster( const uint32_t id );
        //*}

        /**
         * @name Object Registration
         */
        //*{
        /** 
         * Registers a new distributed object.
         *
         * The assigned identifier is unique across all registered objects. The
         * object gets referenced.
         *
         * @todo The master node instance has to exist before any getObject()
         *       causes an instanciation request.
         * 
         * @param object the object instance.
         * @param master the master node for the object, can be
         *               <code>NULL</code> for unmanaged objects.
         */
        void registerObject( Object* object, Node* master );

        /** 
         * Access a registered object.
         * 
         * The object will be instanciated locally if necessary. Versioned
         * objects need to have at least one committed version.
         *
         * @param id the object's identifier.
         * @return the object, or <code>NULL</code> if the object is not known.
         */
        Object* getObject( const uint32_t id );

        /** 
         * Deregisters a distributed object.
         * 
         * @param object the object instance.
         */
        void deregisterObject( Object* object );
        //*}
        
    protected:
        /** Registers requests waiting for a return value. */
        eqBase::RequestHandler _requestHandler;

        /** 
         * Instanciate the slave(proxy) instance of a object on this session.
         * 
         * @param type the type identifier of the object.
         * @param data the instance data of the object.
         * @param dataSize the data size.
         * @return the object, or <code>NULL</code> upon error.
         * @sa Object::getInstanceInfo
         */
        virtual Object* instanciateObject( const uint32_t type,
                                             const void* data, 
                                             const uint64_t dataSize );
        /** 
         * Sends a packet to the session's node.
         * 
         * @param packet the packet.
         * @return the success status of the transaction.
         */
        bool send( const Packet& packet ) { return _server->send( packet ); }

        /** The session's identifier. */
        uint32_t _id;
        
    private:
        friend class Node;
        /** The local node managing the session. */
        eqBase::RefPtr<Node> _localNode;

        /** The node hosting the session. */
        eqBase::RefPtr<Node> _server;

        /** The list of nodes known to this session. */
        NodeIDHash< eqBase::RefPtr<Node> > _nodes;

        /** The session's name. */
        std::string _name;

        /** The state (master/client) of this session instance. */
        bool _isMaster;

        /** The distributed master identifier pool. */
        eqBase::IDPool _masterPool;

        /** The local identifier pool. */
        eqBase::IDPool _localPool;

        /** Stores a mapping from a block of identifiers to a master node. */
        struct IDMasterInfo
        {
            uint32_t                            start;
            uint32_t                            end;
            eqBase::RefPtr<Node>                master;
            std::vector< eqBase::RefPtr<Node> > slaves;
        };
        /** The id->master mapping table. */
        std::vector<IDMasterInfo> _idMasterInfos;
        
        /** The registered object, indexed by identifier. */
        IDHash<Object*> _registeredObjects;

        /** The current state of pending object instanciations. */
        IDHash<Object::InstState> _objectInstStates;

        eqBase::RefPtr<Node> _pollIDMaster( const uint32_t id );

    public: // TODO make private
        void _addRegisteredObject( const uint32_t id, Object* object );
    private:
        CommandResult _handleObjectCommand( Node* node, const Packet* packet );
        CommandResult   _instObject( const uint32_t id );
        void              _sendInitObject( const uint32_t objectID, 
                                            eqBase::RefPtr<Node> master );

        /** The command handler functions. */
        CommandResult _cmdGenIDs( Node* node, const Packet* packet );
        CommandResult _cmdGenIDsReply( Node* node, const Packet* packet );
        CommandResult _cmdSetIDMaster( Node* node, const Packet* packet );
        CommandResult _cmdGetIDMaster( Node* node, const Packet* packet );
        CommandResult _cmdGetIDMasterReply( Node* node, const Packet* packet );
        CommandResult _cmdGetObjectMaster( Node* node, const Packet* packet );
        CommandResult _cmdGetObjectMasterReply( Node* node, const Packet* pkg);
        CommandResult _cmdGetObject( Node* node, const Packet* packet );
        CommandResult _cmdInitObject( Node* node, const Packet* packet );
        CommandResult _cmdInstanciateObject( Node* node, const Packet* packet);
        CommandResult _cmdInitObjectReply( Node* node, const Packet* packet );
    };
    std::ostream& operator << ( std::ostream& os, Session* session );
}
#endif // EQNET_SESSION_PRIV_H

