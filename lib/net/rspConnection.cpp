
/* Copyright (c) 2009, Cedric Stalder <cedric.stalder@gmail.com>
 *                     Stefan Eilemann <eile@equalizergraphics.com>
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
#include "rspConnection.h"

#include "connection.h"
#include "connectionDescription.h"
#include "global.h"
#include "log.h"

#include <eq/base/sleep.h>

//#define EQ_INSTRUMENT_RSP
#define SELF_INTERRUPT 42

#ifdef WIN32
#  define SELECT_TIMEOUT WAIT_TIMEOUT
#  define SELECT_ERROR   WAIT_FAILED
#else
#  define SELECT_TIMEOUT  0
#  define SELECT_ERROR   -1
#endif

#ifndef INFINITE
#  define INFINITE -1
#endif

namespace eq
{
namespace net
{

static const size_t _nBuffers = 4;

static int32_t _mtu = -1;
static int32_t _ackFreq = -1;
uint32_t RSPConnection::_payloadSize = -1;
size_t   RSPConnection::_bufferSize = -1;
uint32_t RSPConnection::_maxNAck = -1;

namespace
{
#ifdef EQ_INSTRUMENT_RSP
base::a_int32_t nReadDataAccepted;
base::a_int32_t nReadData;
base::a_int32_t nBytesRead;
base::a_int32_t nBytesWritten;
base::a_int32_t nDatagrams;
base::a_int32_t nTotalDatagrams;
base::a_int32_t nAckRequests;
base::a_int32_t nTotalAckRequests;
base::a_int32_t nAcksSend;
base::a_int32_t nAcksSendTotal;
base::a_int32_t nAcksRead;
base::a_int32_t nAcksAccepted;
base::a_int32_t nNAcksSend;
base::a_int32_t nNAcksRead;
base::a_int32_t nNAcksResend;
base::a_int32_t nTimeOuts;
base::a_int32_t nTimeInWrite;
base::a_int32_t nTimeInWriteWaitAck;
base::a_int32_t nTimeInReadSync;
base::a_int32_t nTimeInReadData;
base::a_int32_t nTimeInHandleData;
#endif
}



RSPConnection::RSPConnection()
        : _countAcceptChildren( 0 )
        , _id( 0 )
        , _shiftedID( 0 )
        , _timeouts( 0 )
        , _ackReceived( std::numeric_limits< uint16_t >::max( ))
#ifdef WIN32
        , _hEvent( 0 )
#endif
        , _writing( false )
        , _numWriteAcks( 0 )
        , _thread ( 0 )
        , _connection( 0 )
        , _parent( 0 )
        , _lastSequenceIDAck( -1 )
        , _recvBufferIndex( 0 )
        , _readBufferIndex( 0 )
        , _nDatagrams( 0 )
        , _sequenceID( 0 )
{
    _buildNewID();
    _description->type = CONNECTIONTYPE_RSP;
    _description->bandwidth = 102400;

    if( _mtu == -1 )
    {
        _mtu = Global::getIAttribute( Global::IATTR_UDP_MTU );
        _ackFreq =  Global::getIAttribute( Global::IATTR_UDP_PACKET_RATE );
        _payloadSize = _mtu - sizeof( RSPConnection::DatagramData );
        _bufferSize = _payloadSize * _ackFreq;
        _maxNAck = (_mtu - sizeof( RSPConnection::DatagramNack )) / 
                   sizeof( uint32_t );
    }

    for( size_t i = 0; i < _nBuffers; ++i )
        _inBuffers.push_back( new InBuffer );
    
    _recvBuffer = _inBuffers[ _recvBufferIndex ];
    _nackBuffer.reserve( _mtu );

    EQLOG( net::LOG_RSP ) << "New RSP Connection, buffer size " << _bufferSize
                          << ", packet size " << _mtu << std::endl;
}
RSPConnection::~RSPConnection()
{
    close();

    _recvBuffer = 0;

    for( std::vector< InBuffer* >::iterator i = _inBuffers.begin(); 
         i < _inBuffers.end(); ++i )
    {
        delete *i;
    }

    _inBuffers.clear();
}

void RSPConnection::close()
{
    if( _state == STATE_CLOSED )
        return;

    _state = STATE_CLOSED;

    if( _thread )
    {
        const DatagramNode exitNode ={ ID_EXIT, _id };
        _connection->write( &exitNode, sizeof( DatagramNode ) );
        _connectionSet.interrupt();
        _thread->join();
    }

    _setEvent();

    for ( std::vector< RSPConnectionPtr >::iterator i = _children.begin();
        i != _children.end(); ++i )
    {
        RSPConnectionPtr connection = *i;
        connection = 0;
    }

    _parent = 0;

    if( _connection.isValid( ))
        _connection->close();

    _connection = 0;

    for( std::vector< InBuffer* >::iterator i = _inBuffers.begin(); 
         i < _inBuffers.end(); ++i )
    {
        InBuffer* buffer = *i;
        buffer->reset();
    }

    _recvBuffer = _inBuffers[0];
    _fireStateChanged();
}

void RSPConnection::InBuffer::reset() 
{
    sequenceID = 0;
    ackSend    = true;
    allRead    = true;
    readPos    = 0;

    got.resize( _ackFreq );
    data.resize( _bufferSize );
    memset( got.getData(), false, got.getSize( ));
}

//----------------------------------------------------------------------
// Async IO handles
//----------------------------------------------------------------------
#ifdef WIN32
void RSPConnection::_initAIORead()
{
    _hEvent = CreateEvent( 0, TRUE, FALSE, 0 );
    EQASSERT( _hEvent );

    if( !_hEvent )
        EQERROR << "Can't create event for AIO notification: " 
                << base::sysError << std::endl;
}

void RSPConnection::_exitAIORead()
{
    if( _hEvent )
    {
        CloseHandle( _hEvent );
        _hEvent = 0;
    }
}
#else
void RSPConnection::_initAIORead(){ /* NOP */ }
void RSPConnection::_exitAIORead(){ /* NOP */ }
#endif

void RSPConnection::_setEvent()
{
#ifdef WIN32
    SetEvent( _hEvent );
#else
    if( !_selfPipeHEvent->hasData( ))
    {
        const char c = SELF_INTERRUPT;
        _selfPipeHEvent->send( &c, 1, true );
    }
#endif
}

void RSPConnection::_resetEvent()
{
#ifdef WIN32
    ResetEvent( _hEvent );
#else
    while( _selfPipeHEvent->hasData( ))
    {
        _selfPipeHEvent->recvSync( 0, 0 );
        _selfPipeHEvent->recvNB( &_selfCommand, sizeof( _selfCommand ));
    }
#endif
}

RSPConnection::ID RSPConnection::_buildNewID()
{
    _id = _rng.get<ID>();
    _shiftedID = static_cast< ID >( _id ) << ( sizeof( ID ) * 8 );
    return _id;
}

bool RSPConnection::listen()
{
    EQASSERT( _description->type == CONNECTIONTYPE_RSP );

    if( _state != STATE_CLOSED )
        return false;
    
    _state = STATE_CONNECTING;
    _fireStateChanged();

    // init an udp Connection
    _connection = new UDPConnection();
    ConnectionDescriptionPtr description = 
        new ConnectionDescription( *_description.get( ));
    description->type = CONNECTIONTYPE_UDP;
    _connection->setDescription( description );

    // connect UDP multicast
    if( !_connection->connect() )
    {
        EQWARN << "can't connect RSP transmission " << std::endl;
        return false;
    }

    EQASSERT( _mtu == _connection->getMTU( ));
    EQASSERT( _ackFreq =  _connection->getPacketRate( ));

    _description = new ConnectionDescription( *description.get( ));
    _description->type = CONNECTIONTYPE_RSP;

    _connectionSet.addConnection( _connection.get( ));

    // init a thread for manage the communication protocol 
    _thread = new Thread( this );

    _numWriteAcks =  0;
#ifdef WIN32
    _initAIOAccept();
#else
    _selfPipeHEvent = new PipeConnection;
    if( !_selfPipeHEvent->connect( ))
    {
        EQERROR << "Could not create connection" << std::endl;
        return false;
    }

    _hEvent.events = POLLIN;
    _hEvent.fd = _selfPipeHEvent->getNotifier();
    _readFD = _hEvent.fd;
    _hEvent.revents = 0;
    _selfPipeHEvent->recvNB( &_selfCommand, sizeof( _selfCommand ));
#endif
    _readBuffer.resize( _connection->getMTU( ));

    // waits until RSP protocol establishes connection to the multicast network
    if( !_thread->start( ))
    {
        close();
        return false;
    }

    _state = STATE_LISTENING;
    _fireStateChanged();

    EQINFO << "Listening on " << _description->getHostname() << ":"
           << _description->port << " (" << _description->toString() << " @"
           << (void*)this << ")" << std::endl;
    return true;
}



ConnectionPtr RSPConnection::acceptSync()
{
    CHECK_THREAD( _recvThread );
    if( _state != STATE_LISTENING )
        return 0;

    EQASSERT ( _countAcceptChildren < static_cast< int >( _children.size( )));

    RSPConnectionPtr newConnection = _children[ _countAcceptChildren ];

    newConnection->_initAIORead();
    newConnection->_parent      = this;
    newConnection->_connection  = 0;
    newConnection->_state       = STATE_CONNECTED;
    newConnection->_description = _description;

#ifndef WIN32
    newConnection->_selfPipeHEvent = new PipeConnection;
    if( !newConnection->_selfPipeHEvent->connect( ))
    {
        EQERROR << "Could not create connection" << std::endl;
        return false;
    }
    
    newConnection->_hEvent.events = POLLIN;
    newConnection->_hEvent.fd = newConnection->_selfPipeHEvent->getNotifier();
    newConnection->_readFD = newConnection->_hEvent.fd;
    newConnection->_hEvent.revents = 0;
    newConnection->_selfPipeHEvent->recvNB( &newConnection->_selfCommand, 
                                            sizeof( _selfCommand ));

#endif

    ++_countAcceptChildren;
    _sendDatagramCountNode();
    
    EQINFO << "accepted RSP connection " << newConnection->_id << std::endl;
    base::ScopedMutex mutexConn( _mutexConnection );

    if ( static_cast< int >( _children.size() ) > _countAcceptChildren )
        _setEvent();
    else 
        _resetEvent();

    ConnectionPtr connection = newConnection.get();
    return connection;
}

int64_t RSPConnection::readSync( void* buffer, const uint64_t bytes )
{
#ifdef EQ_INSTRUMENT_RSP
    base::Clock clock;
    clock.reset();
#endif
    const uint32_t size = EQ_MIN( bytes, _bufferSize );
    InBuffer* readBuffer = _inBuffers[ _readBufferIndex ];
    
    readBuffer->ackSend.waitEQ( true );
    readBuffer->allRead.waitEQ( false );

    const int64_t sizeRead = _readSync( readBuffer, buffer, size );
#ifdef EQ_INSTRUMENT_RSP
    nTimeInReadSync += clock.getTime64();
    nBytesRead += sizeRead;
#endif

    return sizeRead;
}

bool RSPConnection::_acceptID()
{
    _connection->readNB( _readBuffer.getData(), _mtu );

    // send a first datagram for announce me and discover other connection 
    EQLOG( LOG_RSP ) << "Announce " << _id << std::endl;
    const DatagramNode newnode ={ ID_HELLO, _id };
    _connection->write( &newnode, sizeof( DatagramNode ) );
    _timeouts = 0;
    while ( true )
    {
        switch( _connectionSet.select( 10 ))
        {
            case ConnectionSet::EVENT_TIMEOUT:
                ++_timeouts;
                if ( _timeouts < 20 )
                {
                    EQLOG( LOG_RSP ) << "Announce " << _id << std::endl;
                    const DatagramNode ackNode ={ ID_HELLO, _id };
                    _connection->write( &ackNode, sizeof( DatagramNode ) );
                }
                else 
                {
                    EQLOG( LOG_RSP ) << "Confirm " << _id << std::endl;
                    EQINFO << "opened RSP connection " << _id << std::endl;
                    const DatagramNode confirmNode ={ ID_CONFIRM, _id };
                    _connection->write( &confirmNode, sizeof( DatagramNode ) );
                    _addNewConnection( _id );
                    return true;
                }
                break;

            case ConnectionSet::EVENT_DATA:
                if( !_handleAcceptID() )
                {
                    EQERROR << " Error during Read UDP Connection" 
                            << std::endl;
                    return false;
                }

                _connection->readNB( _readBuffer.getData(), _mtu );
                break;

            case ConnectionSet::EVENT_INTERRUPT:
            default:
                break;
        }
    }
}

bool RSPConnection::_handleAcceptID()
{
    // read datagram 
    if( _connection->readSync( _readBuffer.getData(), _mtu ) == -1 )
    {
        EQERROR << "Error read on Connection UDP" << std::endl;
        return false;
    }

    // read datagram type
    const uint16_t* type = reinterpret_cast<uint16_t*>( _readBuffer.getData() );
    const DatagramNode* node = reinterpret_cast< const DatagramNode* >
                                                      ( _readBuffer.getData( ));
    switch ( *type )
    {
    case ID_HELLO:
        _checkNewID( node->connectionID );
        return true;

    case ID_DENY:
        // a connection refused my ID, try another ID
        if( node->connectionID == _id ) 
        {
            _timeouts = 0;
            const DatagramNode newnode = { ID_HELLO, _buildNewID() };
            EQLOG( LOG_RSP ) << "Announce " << _id << std::endl;
            _connection->write( &newnode, sizeof( DatagramNode ));
        }
        return true;

    case ID_EXIT:
        return _acceptRemoveConnection( node->connectionID );

    default: break;
    
    }

    return true;
}

bool RSPConnection::_initReadThread()
{
    // send a first datagram to announce me and discover other connections 
    _sendDatagramCountNode();
    _timeouts = 0;
    while ( true )
    {
        switch( _connectionSet.select( 10 ) )
        {
            case ConnectionSet::EVENT_TIMEOUT:
                ++_timeouts;
                if( _timeouts >= 20 )
                    return true;

                _sendDatagramCountNode();
                break;

            case ConnectionSet::EVENT_DATA:
                if ( !_handleInitData() )
                {
                    EQERROR << " Error during Read UDP Connection" 
                            << std::endl;
                    return false;
                }

                _connection->readNB( _readBuffer.getData(), _mtu );
                break;

            case ConnectionSet::EVENT_INTERRUPT:
            default:
                break;
        }
    }
}

bool RSPConnection::_handleInitData()
{
    // read datagram 
    if( _connection->readSync( _readBuffer.getData(), _mtu ) == -1 )
    {
        EQERROR << "Read error" << std::endl;
        return false;
    }

    // read datagram type
    const uint16_t* type = reinterpret_cast<uint16_t*>( _readBuffer.getData() );
    const DatagramNode* node = reinterpret_cast< const DatagramNode* >
                                                      ( _readBuffer.getData( ));
    switch ( *type )
    {
    case ID_HELLO:
        _timeouts = 0;
        _checkNewID( node->connectionID ) ;
        return true;

    case ID_CONFIRM:
        _timeouts = 0;
        return _addNewConnection( node->connectionID );

    case COUNTNODE:
    {
            if( _handleCountNode( ))
            _timeouts = 20;
            else
            _timeouts = 0;
    }
    
    case ID_EXIT:
        return _acceptRemoveConnection( node->connectionID );

        default:
            EQUNIMPLEMENTED;
            break;
    }
    
    return true;
}

void* RSPConnection::Thread::run()
{
    _connection->_runReadThread();
    _connection = 0;
    return 0;
}

void RSPConnection::_runReadThread()
{
    EQINFO << "Started RSP read thread" << std::endl;
    while ( _state != STATE_CLOSED && _countAcceptChildren )
    {
        const int32_t timeOut = ( _writing && _repeatQueue.isEmpty( )) ? 
            Global::getIAttribute( Global::IATTR_RSP_ACK_TIMEOUT ) : -1;

        const ConnectionSet::Event event = _connectionSet.select( timeOut );
        switch( event )
        {
        case ConnectionSet::EVENT_TIMEOUT:
        {
#ifdef EQ_INSTRUMENT_RSP
            ++nTimeOuts;
#endif
            ++_timeouts;
            if( _timeouts >= 
                Global::getIAttribute( Global::IATTR_RSP_MAX_TIMEOUTS ))
            {
                    EQERROR << "Error during send, too many timeouts "
                            << _timeouts << std::endl;

                // unblock and terminate write function
                _repeatQueue.push( RepeatRequest( RepeatRequest::DONE ));
                while( _writing )
                        base::sleep( 1 );

                _connection = 0;
                return;
            }
            
            // repeat ack request
            _repeatQueue.push( RepeatRequest( RepeatRequest::ACKREQ ));
            break;
        }

        case ConnectionSet::EVENT_DATA:
        {
#ifdef EQ_INSTRUMENT_RSP
            base::Clock clock;
#endif            
            if ( !_handleData() )
               return;

            _connection->readNB( _readBuffer.getData(), _mtu );
#ifdef EQ_INSTRUMENT_RSP
            nTimeInHandleData += clock.getTime64();
#endif
            break;
        }

        case ConnectionSet::EVENT_INTERRUPT:
            break;
        default: 
            return;
        }
    }
}

int64_t RSPConnection::_readSync( InBuffer* readBuffer, void* buffer, 
                                  const uint64_t bytes )
{
    const uint64_t size = EQ_MIN( bytes, readBuffer->data.getSize() - 
                                  readBuffer->readPos );
    const uint8_t* data = readBuffer->data.getData() + readBuffer->readPos;
    memcpy( buffer, data, size );

    readBuffer->readPos += size;
    
    // if all data in the buffer has been taken
    if( readBuffer->readPos == readBuffer->data.getSize( ))
    {
        EQLOG( net::LOG_RSP ) << "reset read buffer" << std::endl;
        memset( readBuffer->got.getData(), 0, readBuffer->got.getSize( ));
        
        _readBufferIndex = ( _readBufferIndex + 1 ) % _nBuffers;
        {
            base::ScopedMutex mutexEvent( _mutexEvent );
            if( _inBuffers[ _readBufferIndex ]->ackSend == true && 
                _inBuffers[ _readBufferIndex ]->allRead == false )
            {
                _setEvent();
            }
            else
                _resetEvent();

            readBuffer->data.setSize( 0 );
            readBuffer->allRead = true;
        }
    }

    return size;
}

bool RSPConnection::_handleData( )
{
    // read datagram 
    if( _connection->readSync( _readBuffer.getData(), _mtu ) == -1 )
    {
        EQERROR << "Error read on Connection UDP" << std::endl;
        return false;
    }

    // read datagram type
    const uint16_t* type = reinterpret_cast<uint16_t*>( _readBuffer.getData( )); 
    switch ( *type )
    {
    case DATA:
        {

#ifdef EQ_INSTRUMENT_RSP
            base::Clock clock;
            clock.reset();
#endif
            bool resultRead = _handleDataDatagram( 
              reinterpret_cast< const DatagramData* >( _readBuffer.getData( )));

#ifdef EQ_INSTRUMENT_RSP
            nTimeInReadData += clock.getTime64();
#endif
            return resultRead;
        }

    case ACK:
        return _handleAck( reinterpret_cast< const DatagramAck* >( type )) ;
    
    case NACK:
        return _handleNack( reinterpret_cast< const DatagramNack* >
                                                     ( _readBuffer.getData() ));
    
    case ACKREQ: // The writer ask for an ack/nack
        return _handleAckRequest(
                reinterpret_cast< const DatagramAckRequest* >( type ));
    
    case ID_HELLO:
    {
        const DatagramNode* node = reinterpret_cast< const DatagramNode* >
                                                     (  _readBuffer.getData() );
        _checkNewID( node->connectionID );
        return true;
    }

    case ID_CONFIRM:
    {
        const DatagramNode* node = reinterpret_cast< const DatagramNode* >
                                                     (  _readBuffer.getData() );
        return _addNewConnection( node->connectionID );
    }

    case ID_EXIT:
    {
        const DatagramNode* node = reinterpret_cast< const DatagramNode* >
                                                    (  _readBuffer.getData()  );
        return _acceptRemoveConnection( node->connectionID );
    }

    case COUNTNODE:
        _handleCountNode();
            return true;

    default: 
        EQUNIMPLEMENTED;
    }

    return true;
}

bool RSPConnection::_handleDataDatagram( const DatagramData* datagram )
{
#ifdef EQ_INSTRUMENT_RSP
    ++nReadData;
#endif
    const uint32_t writerID = datagram->writeSeqID >> 16;
    RSPConnectionPtr connection = _findConnectionWithWriterID( writerID );
    EQASSERT( connection->_id == writerID );

    // it's an unknown connection or when we run netperf client before
    // server netperf
    // TO DO find a solution in this situation
    if( !connection )
    {
        EQASSERTINFO( false, "Can't find connection with id " << writerID );
        return false;
    }

    // if the buffer hasn't been found during previous read or last
    // ack data sequence.
    // find the data corresponding buffer 
    // why we haven't a receiver here:
    // 1: it's a reception data for another connection
    // 2: all buffer was not ready during last ack data
    const uint32_t sequenceID = datagram->writeSeqID  & 0xFFFF;
    if ( !connection->_recvBuffer )
    {
        EQLOG( net::LOG_RSP ) << "No receive buffer available, searching one" 
                              << std::endl;

        connection->_recvBuffer =
            connection->_findReceiverWithSequenceID( sequenceID );

        if( connection->_inBuffers[ connection->_recvBufferIndex ]->allRead ==
            true )
        {
            connection->_recvBuffer =
                connection->_inBuffers[ connection->_recvBufferIndex ];
        }
        else
        {
            // we do not have a free buffer, which means that the receiver is
            // slower then our read thread. This should not happen, because now
            // we'll drop the data and will send a full NACK packet upon the 
            // ack request, causing retransmission even though we'll drop it
            // again
            //EQWARN << "Reader to slow, dropping data" << std::endl;
            return true;
        }
    }

    InBuffer* receive = connection->_recvBuffer;

    // if it's the first datagram 
    if( receive->ackSend == true )
    {
        if ( sequenceID == receive->sequenceID )
            return true;

        // if it's a datagram repetition for an other connection, we have
        // to ignore it
        if ( ( connection->_lastSequenceIDAck == sequenceID ) &&
             connection->_lastSequenceIDAck != -1 )
        {
            return true;
        }

        EQLOG( net::LOG_RSP ) << "receive data from " << writerID 
                              << " sequenceID " << sequenceID << std::endl;
        receive->sequenceID = sequenceID;
        receive->readPos   = 0;
        receive->data.setSize( 0 );
        receive->ackSend   = false;
    }

    const uint64_t index = datagram->dataIDlength >> 16;

    // if it's a repetition and we have the data then we ignore it
    if( receive->got[ index ] )
        return true;

#ifdef EQ_INSTRUMENT_RSP
    ++nReadDataAccepted;
#endif
    
    const uint16_t length = datagram->dataIDlength & 0xFFFF ; 
    const uint8_t* data = reinterpret_cast< const uint8_t* >( ++datagram );
    const uint64_t pos = ( index ) * ( _mtu - sizeof( DatagramData ));
    
    receive->data.grow( index * _payloadSize + length );
    memcpy( receive->data.getData() + pos, data, length );
    receive->got[ index ] = true;

    // control if the previous datagrams has been received
    if ( index <= 0 ) 
        return true;
    
    if( index <= 0 || receive->got[ index - 1 ] )
        return true;

    EQLOG( net::LOG_RSP ) << "send early nack" << std::endl;
    const uint16_t indexMax = index-1;
    uint16_t indexMin = indexMax;
    
    while( indexMin != 0 )
    {
        if ( !receive->got[ indexMin - 1 ] )
        {
            indexMin--;
            continue;
        }   
        
        break;
    }
    const uint32_t repeatID = indexMax | ( indexMin << 16 ) ; 
    
    _sendNack( writerID, sequenceID, 1, &repeatID );
    return true;
}

bool RSPConnection::_handleAck( const DatagramAck* ack )
{
#ifdef EQ_INSTRUMENT_RSP
    ++nAcksRead;
#endif
    EQLOG( LOG_RSP ) << "Receive Ack from " << ack->readerID << " for "
                     << ack->writerID << " for sequence " << ack->sequenceID
                     << " current " << _sequenceID << std::endl;

    // ignore sequenceID which is different from my write sequence id
    //  Reason : - repeated, late ack or an ack for an other writer
    if( !_isCurrentSequence( ack->sequenceID, ack->writerID ) )
    {
        EQLOG( net::LOG_RSP ) << "ignore Ack, it's not for me" << std::endl;
        return true;
    }

    // find connection destination and if we have not received an ack from it,
    // we update the ack data.
    RSPConnectionPtr connection = _findConnectionWithWriterID( ack->readerID );

    if ( !connection.isValid() )
    {
        EQUNREACHABLE;
        return false;
    }

    // if I have received an ack previously from the reader
    if( connection->_ackReceived == ack->sequenceID )
        return true;

#ifdef EQ_INSTRUMENT_RSP
    ++nAcksAccepted;
#endif
    connection->_ackReceived = ack->sequenceID;
    ++_numWriteAcks;

    // reset timeout counter
    _timeouts = 0;

    if ( _numWriteAcks != _children.size() )
        return true;

    EQLOG( net::LOG_RSP ) << "unlock write function " << _sequenceID 
                          << std::endl;

    // unblock write function if all acks have been received
    _repeatQueue.push( RepeatRequest( RepeatRequest::DONE ));
    return true;
}

bool RSPConnection::_handleNack( const DatagramNack* nack )
{
#ifdef EQ_INSTRUMENT_RSP
    nNAcksRead += nack->count;
#endif

    EQLOG( net::LOG_RSP ) << "handle nack from " << nack->readerID
                          << " for " << nack->writerID << " for sequence "
                          << nack->sequenceID << std::endl;

    RSPConnectionPtr connection = _findConnectionWithWriterID( nack->readerID );

    if( connection->_ackReceived == nack->sequenceID )
    {
        EQLOG( net::LOG_RSP ) << "ignore nack, we received an ack before" 
                              << std::endl;
        return true;
    }

    if( !_isCurrentSequence( nack->sequenceID, nack->writerID ) )
    {
        EQLOG( net::LOG_RSP ) << "ignore nack, it's not for me" << std::endl;
        return true;
    }
    
    if ( !connection )
    {
        EQUNREACHABLE;
        return false;
        // it's an unknown connection 
        // TO DO add this connection?
    }

    _timeouts = 0;

    EQLOG( net::LOG_RSP ) << "Queue data repeat request" << std::endl;
    const uint16_t count = nack->count;
    ++nack;

    _addRepeat( reinterpret_cast< const uint32_t* >( nack ), count );
    return true;
}

void RSPConnection::_addRepeat( const uint32_t* repeatIDs, uint32_t size )
{
    for( size_t j = 0; j < size; ++j )
    {
        RepeatRequest repeat;
        repeat.start = ( repeatIDs[j] & 0xFFFF0000) >> 16;
        repeat.end   = ( repeatIDs[j] & 0xFFFF );
        _repeatQueue.push( repeat );

        EQASSERT( repeat.end <= _nDatagrams );
        EQASSERT( repeat.start <= repeat.end);
    }
}

bool RSPConnection::_handleAckRequest( const DatagramAckRequest* ackRequest )
{
    EQLOG( net::LOG_RSP ) << "received an ack request from " 
                          << ackRequest->writerID << std::endl;
    RSPConnectionPtr connection = 
                            _findConnectionWithWriterID( ackRequest->writerID );

    if ( !connection.isValid() )
    {
        EQUNREACHABLE;
        return false;
    }
    // find the corresponding buffer
    InBuffer* receive = connection->_findReceiverWithSequenceID( 
                                               ackRequest->sequenceID );
    
    // Why no receiver found ?
    // 1 : all datagram data has not been received ( timeout )
    // 2 : all receivers are full and not ready to receive data
    //    We ask for resend of all datagrams
    if ( !receive )
    {
        EQLOG( net::LOG_RSP )
            << "receiver not found, ask to repeat all datagrams" << std::endl;

        const uint32_t repeatID = ackRequest->lastDatagramID; 
        _sendNack( connection->_id, ackRequest->sequenceID, 1, &repeatID );
        return true;
    }
    
    EQLOG( net::LOG_RSP ) << "receiver found " << std::endl;

    // Repeat ack
    if ( receive->ackSend.get() )
    {
        EQLOG( net::LOG_RSP ) << "Repeat Ack for sequence: " 
                              << ackRequest->sequenceID << std::endl;
        _sendAck( ackRequest->writerID, ackRequest->sequenceID );
        return true;
    }
    
    // find all lost datagrams
    EQASSERT( ackRequest->lastDatagramID < receive->got.getSize( ));
    eq::base::Buffer< uint32_t > bufferRepeatID;

    for( size_t i = 0; i <= ackRequest->lastDatagramID; i++)
    {
        // size max datagram = mtu
        if( _maxNAck <= bufferRepeatID.getSize() )
            break;

        if( receive->got[i] )
            continue;
        
        EQLOG( net::LOG_RSP ) << "receiver Nack start " << i << std::endl;
        
        const uint32_t start = i << 16;
        uint32_t end = ackRequest->lastDatagramID;
        
        // OPT: Send all NACK packets at once
        for( ; i < receive->got.getSize(); i++ )
        {
            if( !receive->got[i] )
                continue;
            end = i-1;
            break;
        }
        EQLOG( net::LOG_RSP ) << "receiver Nack end " << end << std::endl;
        const uint32_t repeatID = end | start ; 
        bufferRepeatID.append( repeatID );
    }
    
    // send negative ack
    if( bufferRepeatID.getSize() > 0 )
    {
        EQLOG( net::LOG_RSP ) << "receiver send Nack to connection "
                              << connection->_id << ", sequence " 
                              << ackRequest->sequenceID << std::endl;
        _sendNack( connection->_id, ackRequest->sequenceID,
                   bufferRepeatID.getSize(), bufferRepeatID.getData( ));
        return true;
    }
    
    // no repeat needed, we send an ack and we prepare the next buffer 
    // receive.
    connection->_recvBuffer = 0;
    
    // Find a free buffer for the next receive
    connection->_recvBufferIndex = 
                    ( connection->_recvBufferIndex + 1 ) % _nBuffers;

    if( connection->_inBuffers[ connection->_recvBufferIndex ]->allRead == true)
    {
        EQLOG( net::LOG_RSP ) << "set next buffer  " << std::endl;
        connection->_recvBuffer = 
                 connection->_inBuffers[ connection->_recvBufferIndex ];
    }
    else
    {
        EQLOG( net::LOG_RSP ) << "can't set next buffer  " << std::endl;
    }

    EQLOG( net::LOG_RSP ) << "receiver send Ack to connection " 
                          << connection->_id << ", sequenceID "
                          << ackRequest->sequenceID << std::endl;

    _sendAck( connection->_id, receive->sequenceID );

#ifdef EQ_INSTRUMENT_RSP
    ++nAcksSend;
#endif

    connection->_lastSequenceIDAck = receive->sequenceID;
    {
        base::ScopedMutex mutexEvent( _mutexEvent );
        EQLOG( net::LOG_RSP ) << "data ready, set event for sequence " 
                              << receive->sequenceID << std::endl;

        // the receiver is ready and can be read by ReadSync
        receive->ackSend = true;
        receive->allRead = false;

        connection->_setEvent();
    }

    return true;
}

bool RSPConnection::_handleCountNode()
{
    const DatagramCountConnection* countConn = 
    reinterpret_cast< const DatagramCountConnection* >( _readBuffer.getData( ));

    EQLOG( LOG_RSP ) << "Got " << countConn->nbClient << " from " 
                     << countConn->clientID << std::endl;
    // we know all connections
    if( _children.size() == countConn->nbClient ) 
        return true;

    RSPConnectionPtr connection = 
        _findConnectionWithWriterID( countConn->clientID );

    if( !connection.isValid() )
        _addNewConnection( countConn->clientID );
    return false;
}

void RSPConnection::_checkNewID( ID id )
{
    if ( id == _id )
    {
        EQLOG( LOG_RSP ) << "Deny " << id << std::endl;
        DatagramNode nodeSend = { ID_DENY, _id };
        _connection->write( &nodeSend, sizeof( DatagramNode ) );
        return;
    }
    
    // look if the new ID exist in another connection
    RSPConnectionPtr child = _findConnectionWithWriterID( id );
    if ( child.isValid() )
    {
        EQLOG( LOG_RSP ) << "Deny " << id << std::endl;
        DatagramNode nodeSend = { ID_DENY, id };
        _connection->write( &nodeSend, sizeof( DatagramNode ) );
    }
    
    return;
}

RSPConnection::InBuffer* RSPConnection::_findReceiverWithSequenceID( 
                                               const uint16_t sequenceID ) const
{
    // find the corresponding buffer
    for( std::vector< InBuffer* >::const_iterator k = _inBuffers.begin(); 
          k != _inBuffers.end(); ++k )
    {
        if( (*k)->sequenceID == sequenceID )
            return *k;
    }
    return 0;
}

RSPConnection::RSPConnectionPtr RSPConnection::_findConnectionWithWriterID( 
                                    const ID writerID )
{
    for( size_t i = 0 ; i < _children.size(); ++i )
    {
        if ( _children[i]->_id == writerID )
            return _children[i];
    }
    return 0;
}


bool RSPConnection::_addNewConnection( ID id )
{
    if ( _findConnectionWithWriterID( id ).isValid() )
        return true;

    RSPConnection* connection  = new RSPConnection();
    connection->_connection    = 0;
    connection->_id            = id;

    // protect the event and child size which can be use at the same time 
    // in acceptSync
    {
        base::ScopedMutex mutexConn( _mutexConnection );
        _children.push_back( connection );
        EQWARN << "new rsp connection " << id << std::endl;
        _setEvent();
    }
    _sendDatagramCountNode();
    return true;
}

bool RSPConnection::_acceptRemoveConnection( const ID id )
{
    EQWARN << "remove connection " << id << std::endl;

    for( std::vector< RSPConnectionPtr >::iterator i = _children.begin(); 
          i != _children.end(); ++i )
    {
        RSPConnectionPtr child = *i;
        if( child->_id == id )
        {
            --_countAcceptChildren;
            child->close();
            _children.erase( i );
            break;
        }
    }
    
    _sendDatagramCountNode();

    if ( _children.size() == 1 )
    {
        --_countAcceptChildren;
        _children[0]->close();
        _children.clear();
    }

    return true;
}
bool RSPConnection::_isCurrentSequence( uint16_t sequenceID, uint16_t writer )
{
    return (( sequenceID == _sequenceID ) && ( writer == _id ));
}

int64_t RSPConnection::write( const void* buffer, const uint64_t bytes )
{
    if( _state != STATE_CONNECTED && _state != STATE_LISTENING )
        return -1;
    
    if ( _parent.isValid() )
        return _parent->write( buffer, bytes );

    const uint64_t size = EQ_MIN( bytes, _bufferSize );

    if ( !_connection.isValid() )
        return -1;

#ifdef EQ_INSTRUMENT_RSP
    base::Clock clock;
    clock.reset();
    nBytesWritten += size;
#endif
    _timeouts = 0;
    _numWriteAcks = 0;

    ++_sequenceID;

    // compute number of datagram
    _nDatagrams = size  / _payloadSize;    
    if ( _nDatagrams * _payloadSize != size )
        ++_nDatagrams;

    uint32_t writSeqID = _shiftedID | _sequenceID;

    EQLOG( net::LOG_RSP ) << "write sequence: " << _sequenceID << ", "
                          << _nDatagrams << " datagrams" << std::endl;

    // send each datagram
    const uint8_t* data = reinterpret_cast< const uint8_t* >( buffer );
    for( size_t i = 0; i < _nDatagrams; ++i )
        _sendDatagram( data, size, writSeqID, i );
#ifdef EQ_INSTRUMENT_RSP
    nDatagrams += _nDatagrams;
#endif

    EQLOG( net::LOG_RSP ) << "Initial write done, send ack request for "
                          << _sequenceID << std::endl;

    _writing = true;
    _connectionSet.interrupt();

#ifdef EQ_INSTRUMENT_RSP
    ++nAckRequests;
    base::Clock clockAck;
    clockAck.reset();
#endif

    _sendAckRequest();
    _adaptSendRate( _handleRepeat( data, size ));

#ifdef EQ_INSTRUMENT_RSP
    nTimeInWriteWaitAck  += clockAck.getTime64();
    nTimeInWrite += clock.getTime64();

    if( bytes <= _bufferSize )
        EQWARN << *this << std::endl;
#endif

    EQLOG( net::LOG_RSP ) << "wrote sequence " << _sequenceID << std::endl;
    return size;
}

int64_t RSPConnection::_handleRepeat( const uint8_t* data, const uint64_t size )
{
    const uint32_t writeSeqID = _shiftedID | _sequenceID;
    int64_t nRepeats = 0;

    while( true )
    {
        std::vector< RepeatRequest > requests;

        while( requests.empty( ))
        {
            const RepeatRequest& request = _repeatQueue.pop();
            switch( request.type )
            {
                case RepeatRequest::DONE:
                    _writing = false;
                    return nRepeats;

                case RepeatRequest::ACKREQ:
                    _sendAckRequest();
                    _connectionSet.interrupt();
                    break;

                case RepeatRequest::NACK:
                {
                    requests.push_back( request );
                    const int32_t time = 
                        Global::getIAttribute( Global::IATTR_RSP_NACK_DELAY );
                    if( time > 0 )
                        base::sleep( time );
                    break;
                }

                default:
                    EQUNIMPLEMENTED;
            }
        }

        // merge nack requests
        while( !_repeatQueue.isEmpty( ))
        {
            const RepeatRequest& candidate = _repeatQueue.pop();
            switch( candidate.type )
            {
                case RepeatRequest::DONE:
                    _writing = false;
                    return nRepeats;

                case RepeatRequest::ACKREQ:
                    break; // ignore, will send one below anyway

                case RepeatRequest::NACK:
                {
                    bool merged = false;
                    for( std::vector< RepeatRequest >::iterator i = 
                         requests.begin(); i != requests.end() && !merged; ++i )
                    {
                        RepeatRequest& old = *i;
                        if( old.start <= candidate.end &&
                            old.end   >= candidate.start )
                        {
                            old.start = EQ_MIN( old.start, candidate.start);
                            old.end   = EQ_MAX( old.end, candidate.end );
                            merged    = true;
                        }
                    }

                    if( !merged )
                        requests.push_back( candidate );
                }
            }
        }

        // calculate errors and adapt send rate
        uint64_t errors = 0;
        for( std::vector< RepeatRequest >::iterator i = requests.begin();
             i != requests.end(); ++i )
        {
            RepeatRequest& repeat = *i; 
            errors += repeat.end - repeat.start + 1;
            ++nRepeats;
        }
        _adaptSendRate( errors );

        // send merged requests
        for( std::vector< RepeatRequest >::iterator i = requests.begin();
             i != requests.end(); ++i )
        {
#ifdef EQ_INSTRUMENT_RSP
            ++nNAcksResend;
#endif
            RepeatRequest& repeat = *i;        
            EQASSERT( repeat.start <= repeat.end );
            EQLOG( LOG_RSP ) << "Repeat " << repeat.start << ".." << repeat.end
                             << std::endl;

            for( size_t j = repeat.start; j <= repeat.end; ++j )
                _sendDatagram( data, size, writeSeqID, j );

        }

        // re-request ack
        if ( _repeatQueue.isEmpty() )
        {
            _sendAckRequest( );
            _connectionSet.interrupt();
        }
    }
}

void RSPConnection::_adaptSendRate( const uint64_t errors )
{
    const float error = ( static_cast< float >( errors ) /
                          static_cast< float >( _nDatagrams ) * 100.f ) - 
        Global::getIAttribute( Global::IATTR_RSP_ERROR_BASE_RATE );

    if ( error < 0.f )
    {
        int32_t delta = static_cast< int32_t >( error *
                      Global::getIAttribute( Global::IATTR_RSP_ERROR_UPSCALE ));
        delta = EQ_MIN( Global::getIAttribute( Global::IATTR_RSP_ERROR_MAX ),
                        delta );

        EQLOG( LOG_RSP ) << errors << "/" << _nDatagrams
                         << " errors, change send rate by " << -delta << "%"
                         << std::endl;
        _connection->adaptSendRate( -delta );
    }
    else
    {
        int32_t delta = static_cast< int32_t >( error /
                    Global::getIAttribute( Global::IATTR_RSP_ERROR_DOWNSCALE ));
        delta = EQ_MIN( Global::getIAttribute( Global::IATTR_RSP_ERROR_MAX ),
                        delta );

        EQLOG( LOG_RSP ) << errors << "/" << _nDatagrams
                         << " errors, change send rate by " << -delta << "%"
                         << std::endl;
        _connection->adaptSendRate( -delta );
    }
}

void RSPConnection::_sendDatagramCountNode()
{
    if ( !_findConnectionWithWriterID( _id ) )
        return;

    EQLOG( LOG_RSP ) << _children.size() << " nodes" << std::endl;
    const DatagramCountConnection count = { COUNTNODE, _id, _children.size() };
    _connection->write( &count, sizeof( count ));
}

void RSPConnection::_sendAck( const ID writerID, const uint16_t sequenceID )
{
#ifdef EQ_INSTRUMENT_RSP
    ++nAcksSendTotal;
#endif
    const DatagramAck ack = { ACK, _id, writerID, sequenceID };
    if ( _id == writerID )
        _handleAck( &ack );
    else
        _connection->write( &ack, sizeof( ack ) );
}

void RSPConnection::_sendNack( const ID toWriterID, const uint16_t sequenceID,
                               const uint8_t count, const uint32_t* repeatID )
{
#ifdef EQ_INSTRUMENT_RSP
    ++nNAcksSend;
#endif
    /* optimization : we use the direct access to the reader. */
    if ( toWriterID == _id )
    {
         _addRepeat( repeatID, count );
         return;
    }

    const int32_t size = count * sizeof( uint32_t ) + sizeof( DatagramNack );
    EQASSERT( size <= _mtu );

    // set the header
    DatagramNack* header = 
        reinterpret_cast< DatagramNack* >( _nackBuffer.getData( ));

    header->type = NACK;
    header->readerID = _id;
    header->writerID = toWriterID;
    header->sequenceID = sequenceID;
    header->count = count;

    memcpy( header+1, repeatID, size - sizeof( DatagramNack ));
    _connection->write( header, size );
}

void RSPConnection::_sendDatagram( const uint8_t* data, const uint64_t size,
                                   const uint32_t writeSeqID,
                                   const uint16_t idDatagram )
{
#ifdef EQ_INSTRUMENT_RSP
    ++nTotalDatagrams;
#endif
    const uint32_t posInData = _payloadSize * idDatagram;
    
    uint32_t packetSize;
    if( size - posInData >= _payloadSize )
        packetSize = _payloadSize;
    else
        packetSize = size - posInData;
    
    const uint8_t* ptr = data + posInData;
    _sendBuffer.resize( packetSize + sizeof( DatagramData ) );
    
    uint32_t dataIDlength = ( idDatagram << 16 ) | ( packetSize & 0xffff ) ;

    // set the header
    DatagramData* header = reinterpret_cast< DatagramData* >
                                                ( _sendBuffer.getData() );
    header->type = DATA;
    header->writeSeqID = writeSeqID;
    header->dataIDlength = dataIDlength;

    memcpy( header + 1, ptr, packetSize );

    // send Data
    _handleDataDatagram( header );
    _connection->waitWritable( _sendBuffer.getSize( ));
    _connection->write ( header, _sendBuffer.getSize() );
}

void RSPConnection::_sendAckRequest()
{
#ifdef EQ_INSTRUMENT_RSP
    ++nTotalAckRequests;
#endif
    const DatagramAckRequest ackRequest = { ACKREQ, _id, _nDatagrams - 1,
                                            _sequenceID };
    _handleAckRequest( &ackRequest );
    _connection->write( &ackRequest, sizeof( DatagramAckRequest ) );
}

std::ostream& operator << ( std::ostream& os,
                            const RSPConnection& connection )
{
    os << base::disableFlush << base::disableHeader << "RSPConnection id "
       << connection.getID() << " send rate " << connection.getSendRate();

#ifdef EQ_INSTRUMENT_RSP
    os << ": read " << nBytesRead << " bytes, wrote " 
       << nBytesWritten << " bytes using " << nDatagrams << " dgrams "
       << nTotalDatagrams - nDatagrams << " repeated, "
       << nTimeOuts << " write timeouts, "
       << std::endl
       << nAckRequests << " ack requests " 
       << nTotalAckRequests - nAckRequests << " repeated, "
       << nAcksAccepted << "/" << nAcksRead << " acks read, "
       << nNAcksResend << "/" << nNAcksRead << " nacks answered, "
       << std::endl
       << nAcksSend << " acks " << nAcksSendTotal - nAcksSend << " repeated, "
       << nNAcksSend << " negative acks "

       << std::endl
       << " time in write " << nTimeInWrite 
       << " ack wait time  "  << nTimeInWriteWaitAck
       << " nTimeInReadSync " << nTimeInReadSync
       << " nTimeInReadData " << nTimeInReadData
       << " nTimeInHandleData " << nTimeInHandleData;

    nReadDataAccepted = 0;
    nReadData = 0;
    nBytesRead = 0;
    nBytesWritten = 0;
    nDatagrams = 0;
    nTotalDatagrams = 0;
    nAckRequests = 0;
    nTotalAckRequests = 0;
    nAcksSend = 0;
    nAcksSendTotal = 0;
    nAcksRead = 0;
    nAcksAccepted = 0;
    nNAcksSend = 0;
    nNAcksRead = 0;
    nNAcksResend = 0;
    nTimeOuts = 0;
    nTimeInWrite = 0;
    nTimeInWriteWaitAck = 0;
    nTimeInReadSync = 0;
    nTimeInReadData = 0;
    nTimeInHandleData = 0;
#endif
    os << base::enableHeader << base::enableFlush;

    return os;
}

}
}
