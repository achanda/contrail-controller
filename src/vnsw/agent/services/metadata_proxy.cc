/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <boost/bind.hpp>
#include <boost/asio/ip/host_name.hpp>
#include <boost/foreach.hpp>
#include <boost/tokenizer.hpp>
#include <boost/assign/list_of.hpp>

#include <isc/hmacmd5.h>
#include <isc/hmacsha.h>

#include "base/contrail_ports.h"
#include "http/http_request.h"
#include "http/http_session.h"
#include "http/http_server.h"
#include "http/client/http_client.h"
#include "http/client/http_curl.h"
#include "io/event_manager.h"
#include "cmn/agent_cmn.h"
#include "oper/operdb_init.h"
#include "oper/mirror_table.h"
#include "oper/interface_common.h"
#include "oper/global_vrouter.h"
#include "pkt/pkt_handler.h"
#include "services/services_types.h"
#include "services/services_init.h"
#include "services/metadata_proxy.h"
#include "services/services_sandesh.h"

////////////////////////////////////////////////////////////////////////////////

#define METADATA_TRACE(obj, arg)                                               \
do {                                                                           \
    std::ostringstream _str;                                                   \
    _str << arg;                                                               \
    Metadata##obj::TraceMsg(MetadataTraceBuf, __FILE__, __LINE__, _str.str()); \
} while (false)                                                                \

// Get HMAC SHA256 digest
static std::string
GetHmacSha256(const std::string &key, const std::string &data) {
    isc_hmacsha256_t hmacsha256;
    isc_hmacsha256_init(&hmacsha256, (const unsigned char *)key.c_str(),
                        key.length());
    isc_hmacsha256_update(&hmacsha256, (const isc_uint8_t *)data.c_str(),
                          data.length());
    unsigned char hmac_sha256_digest[ISC_SHA512_DIGESTLENGTH];
    isc_hmacsha256_sign(&hmacsha256, hmac_sha256_digest,
                        ISC_SHA256_DIGESTLENGTH);
    std::stringstream str;
    for (unsigned int i = 0; i < ISC_SHA256_DIGESTLENGTH; i++) {
        str << std::hex << std::setfill('0') << std::setw(2)
            << (int) hmac_sha256_digest[i];
    }
    return str.str();
}

////////////////////////////////////////////////////////////////////////////////

MetadataProxy::MetadataProxy(ServicesModule *module,
                             const std::string &secret)
    : services_(module), shared_secret_(secret),
      http_server_(new HttpServer(services_->agent()->GetEventManager())),
      http_client_(new HttpClient(services_->agent()->GetEventManager())) {

    // Register wildcard entry to match any URL coming on the metadata port
    http_server_->RegisterHandler(HTTP_WILDCARD_ENTRY,
        boost::bind(&MetadataProxy::HandleMetadataRequest, this, _1, _2));
    http_server_->Initialize(0);
    services_->agent()->SetMetadataServerPort(http_server_->GetPort());

    http_client_->Init();
}

MetadataProxy::~MetadataProxy() {
}

void
MetadataProxy::Shutdown() {
    http_server_->Shutdown();
    http_server_ = NULL;
    http_client_->Shutdown();
    http_client_ = NULL;
}

void 
MetadataProxy::HandleMetadataRequest(HttpSession *session, const HttpRequest *request) {
    bool conn_close = false;
    std::string msg;
    std::string uri;
    std::string header_options;
    std::string vm_ip, vm_uuid, vm_project_uuid;
    metadata_stats_.requests++;
    boost::asio::ip::address_v4 ip = session->remote_endpoint().address().to_v4();

    if (!services_->agent()->GetInterfaceTable()->
         FindVmUuidFromMetadataIp(ip, &vm_ip, &vm_uuid, &vm_project_uuid)) {
        ErrorClose(session);
        http_server_->DeleteSession(session);
        delete request;
        METADATA_TRACE(Trace, "Metadata GET for VM : " << ip <<
                       " Error : Interface Config not available");
        return;
    }
    std::string signature = GetHmacSha256(shared_secret_, vm_uuid);
    const HttpRequest::HeaderMap &req_header = request->Headers();
    for (HttpRequest::HeaderMap::const_iterator it = req_header.begin();
         it != req_header.end(); ++it) {
        std::string option = boost::to_lower_copy(it->first);
        if (option == "host") {
            continue;
        }
        if (option == "connection") {
            std::string val = boost::to_lower_copy(it->second);
            if (val == "close")
                conn_close = true;
            continue;
        }
        header_options += it->first + ": " + it->second + "\r\n";
    }

    // keystone uses uuids without dashes and that is what ends up in
    // the nova database entry for the instance. Remove dashes from the
    // uuid string representation.
    boost::replace_all(vm_project_uuid, "-", "");
    header_options += "X-Forwarded-For: " + vm_ip + "\r\n" +
                      "X-Instance-ID: " + vm_uuid + "\r\n" +
                      "X-Tenant-ID: " + vm_project_uuid + "\r\n" +
                      "X-Instance-ID-Signature: " + signature;

    bool delete_session = false;
    uri = request->UrlPath().substr(1); // ignore the first "/"
    switch(request->GetMethod()) {
        case HTTP_GET: {
            tbb::mutex::scoped_lock lock(mutex_);
            HttpConnection *conn = GetProxyConnection(session, conn_close);
            if (conn) {
                conn->HttpGet(uri, true, false, header_options,
                boost::bind(&MetadataProxy::HandleMetadataResponse,
                            this, conn, HttpSessionPtr(session), _1, _2));
                METADATA_TRACE(Trace, "Metadata GET for VM : " << vm_ip <<
                               " URL : " << uri);
            } else {
                ErrorClose(session);
                delete_session = true;
                METADATA_TRACE(Trace, "Metadata GET for VM : " << vm_ip <<
                               " Error : Metadata Config not available");
            }
            break;
        }

        default:
            METADATA_TRACE(Trace, "Metadata Unsupported Request Method : " <<
                           request->GetMethod());
            break;
    }

    if (delete_session)
        http_server_->DeleteSession(session);

    delete request;
}

// Metadata Response from Nova API service
void
MetadataProxy::HandleMetadataResponse(HttpConnection *conn, HttpSessionPtr session,
                                      std::string &msg, boost::system::error_code &ec) {
    bool delete_session = false;
    {
        tbb::mutex::scoped_lock lock(mutex_);

        // Ignore if session is closed in the meantime
        SessionMap::iterator it = metadata_sessions_.find(session.get());
        if (it == metadata_sessions_.end())
            return;

        std::string vm_ip, vm_uuid, vm_project_uuid;
        boost::asio::ip::address_v4 ip = session->remote_endpoint().address().to_v4();
        services_->agent()->GetInterfaceTable()->
            FindVmUuidFromMetadataIp(ip, &vm_ip, &vm_uuid, &vm_project_uuid);

        if (!ec) {
            METADATA_TRACE(Trace, "Metadata for VM : " << vm_ip << " Response : " << msg);
            session->Send(reinterpret_cast<const u_int8_t *>(msg.c_str()),
                          msg.length(), NULL);
        } else {
            METADATA_TRACE(Trace, "Metadata for VM : " << vm_ip << " Error : " << 
                                  boost::system::system_error(ec).what());
            CloseClientSession(conn);
            ErrorClose(session.get());
            delete_session = true;
            goto done;
        }

        if (!ec && it->second.close_req) {
            std::stringstream str(msg);
            std::string option;
            str >> option;
            if (option == "Content-Length:") {
                str >> it->second.content_len;
            } else if (msg == "\r\n") {
                it->second.header_end = true;
                if (it->second.header_end && !it->second.content_len) {
                    CloseClientSession(it->second.conn);
                    CloseServerSession(session.get());
                    delete_session = true;
                    metadata_stats_.responses++;
                }
            } else if (it->second.header_end) {
                it->second.data_sent += msg.length();
                if (it->second.data_sent >= it->second.content_len) {
                    CloseClientSession(it->second.conn);
                    CloseServerSession(session.get());
                    delete_session = true;
                    metadata_stats_.responses++;
                }
            }
        }
    }

done:
    if (delete_session) {
        http_server_->DeleteSession(session.get());
    }
}

void
MetadataProxy::OnServerSessionEvent(HttpSession *session, TcpSession::Event event) {
    switch (event) {
        case TcpSession::CLOSE: {
            tbb::mutex::scoped_lock lock(mutex_);
            SessionMap::iterator it = metadata_sessions_.find(session);
            if (it == metadata_sessions_.end())
                break;
            CloseClientSession(it->second.conn);
            metadata_sessions_.erase(it);
            break;
        }

        default:
            break;
    }
}

void
MetadataProxy::OnClientSessionEvent(HttpClientSession *session, TcpSession::Event event) {
    switch (event) {
        case TcpSession::CLOSE: {
            {
                tbb::mutex::scoped_lock lock(mutex_);
                ConnectionSessionMap::iterator it = 
                    metadata_proxy_sessions_.find(session->Connection());
                if (it == metadata_proxy_sessions_.end())
                    break;
                CloseServerSession(it->second);
                CloseClientSession(session->Connection());
            }
            http_server_->DeleteSession(session);
            break;
        }

        default:
            break;
    }
}

HttpConnection *
MetadataProxy::GetProxyConnection(HttpSession *session, bool conn_close) {
    SessionMap::iterator it = metadata_sessions_.find(session);
    if (it != metadata_sessions_.end()) {
        it->second.close_req = conn_close;
        return it->second.conn;
    }

    uint16_t nova_port, linklocal_port;
    Ip4Address nova_server, linklocal_server;
    if (!services_->agent()->oper_db()->global_vrouter()->FindLinkLocalService(
        GlobalVrouter::kMetadataService, &linklocal_server, &linklocal_port,
        &nova_server, &nova_port))
        return NULL;

    boost::asio::ip::tcp::endpoint http_ep;
    http_ep.address(nova_server);
    http_ep.port(nova_port);

    HttpConnection *conn = http_client_->CreateConnection(http_ep);
    conn->RegisterEventCb(
             boost::bind(&MetadataProxy::OnClientSessionEvent, this, _1, _2));
    session->RegisterEventCb(
             boost::bind(&MetadataProxy::OnServerSessionEvent, this, _1, _2));
    SessionData data(conn, conn_close);
    metadata_sessions_.insert(SessionPair(session, data));
    metadata_proxy_sessions_.insert(ConnectionSessionPair(conn, session));
    metadata_stats_.proxy_sessions++;
    return conn;
}

void
MetadataProxy::CloseServerSession(HttpSession *session) {
    session->Close();
    metadata_sessions_.erase(session);
}

void
MetadataProxy::CloseClientSession(HttpConnection *conn) {
    HttpClient *client = conn->client();
    client->RemoveConnection(conn);
    metadata_proxy_sessions_.erase(conn);
}

void
MetadataProxy::ErrorClose(HttpSession *session) {
    const char body[] = "<html>\n"
                        "<head>\n"
                        " <title>404 Not Found</title>\n"
                        "</head>\n"
                        "</html>\n";
    char response[512];
    snprintf(response, sizeof(response),
             "HTTP/1.1 404 Not Found\n"
             "Content-Type: text/html; charset=UTF-8\n"
             "Content-Length: %lu\n"
             "\n%s", strlen(body), body);
    session->Send(reinterpret_cast<const u_int8_t *>(response),
                  strlen(response), NULL);
    CloseServerSession(session);
    metadata_stats_.internal_errors++;
}
