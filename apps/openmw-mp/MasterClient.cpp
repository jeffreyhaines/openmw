//
// Created by koncord on 14.08.16.
//

#include <RakSleep.h>
#include <Getche.h>
#include <sstream>
#include <iostream>
#include <RakPeerInterface.h>
#include "MasterClient.hpp"
#include <components/openmw-mp/Log.hpp>
#include "Networking.hpp"

using namespace std;

bool MasterClient::sRun = false;

MasterClient::MasterClient(std::string queryAddr, unsigned short queryPort, std::string serverAddr,
                           unsigned short serverPort) : queryAddr(queryAddr), queryPort(queryPort),
                                                        serverAddr(serverAddr), serverPort(serverPort)
{
    httpConnection = RakNet::HTTPConnection2::GetInstance();
    tcpInterface.Start(0, 64);
    tcpInterface.AttachPlugin(httpConnection);
    players = 0;
    maxPlayers = 0;
    motd = "";
    timeout = 1000; // every 1 seconds
}

void MasterClient::SetPlayers(unsigned pl)
{
    mutexData.lock();
    players = pl;
    mutexData.unlock();
}

void MasterClient::SetMaxPlayers(unsigned pl)
{
    mutexData.lock();
    maxPlayers = pl;
    mutexData.unlock();
}

void MasterClient::SetMOTD(std::string &motd)
{
    mutexData.lock();
    this->motd = motd.substr(0, 200);
    mutexData.unlock();
}

RakNet::RakString MasterClient::Send(std::string motd, unsigned players, unsigned maxPlayers, bool update)
{
    /*static unsigned short oldServerPort, oldQueryPort;
    static string oldMotd;
    static unsigned oldPlayers, oldMaxPlayers;*/
    std::stringstream sstr;
    mutexData.lock();
    sstr << "{";
    sstr << "\"port\": " << serverPort << ", ";
    sstr << "\"query_port\": " << queryPort << ", ";
    sstr << "\"motd\": \"" << motd << "\", ";
    sstr << "\"players\": " << players << ", ";
    sstr << "\"max_players\": " << maxPlayers;
    sstr << "}";
    mutexData.unlock();

    std::string contentType = "application/json";
    RakNet::RakString createRequest;

    if (update)
        createRequest = RakNet::RakString::FormatForPUT(
                string("/api/servers/" + serverAddr + ":" + to_string(serverPort)).c_str(), contentType.c_str(),
                sstr.str().c_str());
    else
        createRequest = RakNet::RakString::FormatForPOST("/api/servers", contentType.c_str(),
                                                         sstr.str().c_str());

    httpConnection->TransmitRequest(createRequest.C_String(), queryAddr.c_str(), queryPort);

    RakNet::Packet *packet;
    RakNet::SystemAddress sa;

    RakNet::RakString transmitted, hostTransmitted;
    RakNet::RakString response;
    RakNet::SystemAddress hostReceived;
    int contentOffset;

    while (true)
    {

        // This is kind of crappy, but for TCP plugins, always do HasCompletedConnectionAttempt,
        // then Receive(), then HasFailedConnectionAttempt(),HasLostConnection()
        sa = tcpInterface.HasCompletedConnectionAttempt();
        if (sa != RakNet::UNASSIGNED_SYSTEM_ADDRESS)
            LOG_MESSAGE_SIMPLE(Log::LOG_INFO, "Connected to master server: %s", sa.ToString());

        sa = tcpInterface.HasFailedConnectionAttempt();
        if (sa != RakNet::UNASSIGNED_SYSTEM_ADDRESS)
        {
            LOG_MESSAGE_SIMPLE(Log::LOG_WARN, "Failed to connect to master server: %s", sa.ToString());
            return "FAIL_CONNECT";
        }
        sa = tcpInterface.HasLostConnection();
        if (sa != RakNet::UNASSIGNED_SYSTEM_ADDRESS)
        {
            LOG_MESSAGE_SIMPLE(Log::LOG_WARN, "Lost connection to master server: %s", sa.ToString());
            return "LOST_CONNECTION";
        }

        for (packet = tcpInterface.Receive(); packet; tcpInterface.DeallocatePacket(
                packet), packet = tcpInterface.Receive());

        if (httpConnection->GetResponse(transmitted, hostTransmitted, response, hostReceived, contentOffset))
        {
            if (contentOffset < 0)
                return "NO_CONTENT"; // no content
            tcpInterface.CloseConnection(sa);
            return (response.C_String() + contentOffset);
        }
        RakSleep(30);
    }

}

void MasterClient::Update()
{
    assert(!sRun);
    RakNet::RakString response = Send(motd, players, maxPlayers, false);
    bool update = true;
    sRun = true;
    while (sRun)
    {
        if (response == "Created")
            LOG_MESSAGE_SIMPLE(Log::LOG_INFO, "Server registered on the master server.");
        else if (response == "Accepted")
            LOG_MESSAGE_SIMPLE(Log::LOG_INFO, "Server updated information about himself on the master server.");
        else if (response == "bad request")
        {
            LOG_MESSAGE_SIMPLE(Log::LOG_WARN, "Update rate is too low, and the master server has deleted information about"
                    " the server. Trying low rate...");
            SetUpdateRate(timeout - step_rate);
            update = false;
        }
        else
        {
            /*cout << "Error: \""<< response << "\"" << endl;
            cout << response.GetLength() << endl;*/
        }

        RakSleep(timeout);
        ;
        players = mwmp::Networking::get().numberOfConnections();
        response = Send(motd, players, maxPlayers, update);
        update = true;
    }

}

void MasterClient::Stop()
{
    sRun = false;
}

void MasterClient::SetUpdateRate(unsigned int rate)
{
    if (timeout < min_rate)
        timeout = min_rate;
    else if (timeout > max_rate)
        timeout = max_rate;
    timeout = rate;
}