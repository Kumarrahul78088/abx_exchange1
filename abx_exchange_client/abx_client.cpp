#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define delay_milliseconds(x) Sleep(x)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
#else
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #define delay_milliseconds(x) usleep(x*1000)
#endif

#include <iostream>
#include <fstream>
#include <vector>
#include <set>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <errno.h>

// Data structure for message format
struct MarketMessage {
    char assetCode[5];
    char orderDirection;
    int32_t size;
    int32_t cost;
    int32_t sequenceNum;
};

// Visual feedback component
class LoadingIndicator {
    int barWidth;
    float percentComplete;
public:
    LoadingIndicator(int width = 50) : barWidth(width), percentComplete(0) {}

    void show(float percent) {
        percentComplete = percent;
        int filledWidth = barWidth * percentComplete;
        std::cout << "\r[";
        for (int i = 0; i < barWidth; ++i) {
            if (i < filledWidth) std::cout << "=";
            else if (i == filledWidth) std::cout << ">";
            else std::cout << " ";
        }
        std::cout << "] " << int(percentComplete * 100.0) << "%" << std::flush;
    }
};

class MarketDataClient {
private:
    #ifdef _WIN32
        WSADATA wsaData;
        SOCKET socketHandle;
    #else
        int socketHandle;
    #endif

    const char* hostIP;
    const int hostPort;
    std::vector<MarketMessage> messageLog;
    std::set<int> processedSequences;
    std::chrono::steady_clock::time_point sessionStart;

    void logInitialization() {
        std::cout << "[INFO] Market Data Client Started" << std::endl;
    }
    
    void generateReport() {
        auto currentTime = std::chrono::steady_clock::now();
        auto totalRuntime = std::chrono::duration_cast<std::chrono::seconds>(currentTime - sessionStart).count();
    
        std::cout << "\n[INFO] Session Report" << std::endl;
        std::cout << "-----------------------------------" << std::endl;
        std::cout << "Total Messages       : " << messageLog.size() << std::endl;
        std::cout << "Session Duration     : " << totalRuntime << "s" << std::endl;
        std::cout << "Processing Rate      : " << messageLog.size() / (totalRuntime ? totalRuntime : 1) << " msg/s" << std::endl;
    }
    
    bool openConnection() {
        #ifdef _WIN32
            socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socketHandle == INVALID_SOCKET) {
                std::cerr << "* Socket creation error: " << WSAGetLastError() << std::endl;
                return false;
            }
        #else
            socketHandle = socket(AF_INET, SOCK_STREAM, 0);
            if (socketHandle < 0) {
                std::cerr << "* Socket creation error" << std::endl;
                return false;
            }
        #endif

        struct sockaddr_in serverAddress;
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = htons(hostPort);
        serverAddress.sin_addr.s_addr = inet_addr(hostIP);

        if (connect(socketHandle, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
            #ifdef _WIN32
                std::cerr << "* Connection failed: " << WSAGetLastError() << std::endl;
                closesocket(socketHandle);
            #else
                std::cerr << "* Connection failed" << std::endl;
                close(socketHandle);
            #endif
            return false;
        }

        std::cout << "[SUCCESS] Connected to data server" << std::endl;
        return true;
    }

    void sendCommand(uint8_t commandCode, uint8_t sequenceParam = 0) {
        uint8_t commandBuffer[2] = {commandCode, sequenceParam};
        if (send(socketHandle, reinterpret_cast<const char*>(commandBuffer), sizeof(commandBuffer), 0) < 0) {
            std::cerr << "* Failed to send command" << std::endl;
        }
    }

    bool readMessage(MarketMessage& message) {
        uint8_t buffer[17];
        const size_t expectedBytes = sizeof(buffer);
        int bytesReceived = 0;
        size_t totalBytesReceived = 0;
    
        while (totalBytesReceived < expectedBytes) {
            bytesReceived = recv(socketHandle, 
                            reinterpret_cast<char*>(buffer) + totalBytesReceived, 
                            static_cast<int>(expectedBytes - totalBytesReceived), 
                            0);
            
            if (bytesReceived <= 0) {
                if (bytesReceived == 0) {
                    return false; // Connection closed
                }
                #ifdef _WIN32
                    if (WSAGetLastError() == WSAEINTR) continue;
                    std::cerr << "* Data reception error: " << WSAGetLastError() << std::endl;
                #else
                    if (errno == EINTR) continue;
                    std::cerr << "* Data reception error: " << strerror(errno) << std::endl;
                #endif
                return false;
            }
            totalBytesReceived += static_cast<size_t>(bytesReceived);
        }
    
        memcpy(message.assetCode, buffer, 4);
        message.assetCode[4] = '\0';
        message.orderDirection = buffer[4];
        message.size = ntohl(*reinterpret_cast<int32_t*>(buffer + 5));
        message.cost = ntohl(*reinterpret_cast<int32_t*>(buffer + 9));
        message.sequenceNum = ntohl(*reinterpret_cast<int32_t*>(buffer + 13));
        
        return true;
    }

    void logMessage(const MarketMessage& message) {
        messageLog.push_back(message);
        processedSequences.insert(message.sequenceNum);
        std::cout << "[RECEIVED] Message " << message.sequenceNum 
                << " (" << message.assetCode << ")" << std::endl;
    }

    std::string messageToJSON(const MarketMessage& message, bool isLast) {
        std::stringstream output;
        output << "    {\n";
        output << "        \"assetCode\": \"" << message.assetCode << "\",\n";
        output << "        \"orderDirection\": \"" << message.orderDirection << "\",\n";
        output << "        \"size\": " << message.size << ",\n";
        output << "        \"cost\": " << message.cost << ",\n";
        output << "        \"sequenceNum\": " << message.sequenceNum << "\n";
        output << "    }" << (isLast ? "\n" : ",\n");
        return output.str();
    }

    void disconnectServer() {
        #ifdef _WIN32
            closesocket(socketHandle);
        #else
            close(socketHandle);
        #endif
    }

    void fetchMissingData(int maxSequence) {
        std::cout << "\n-> Validating data integrity..." << std::endl;
        
        LoadingIndicator progress;
        int missingCount = 0;
        int recoveredCount = 0;
        
        for (int seq = 1; seq <= maxSequence; ++seq) {
            progress.show(float(seq) / maxSequence);
            
            if (processedSequences.find(seq) == processedSequences.end()) {
                missingCount++;
                std::cout << "\n! Requesting sequence number: " << seq;
                
                if (!openConnection()) {
                    std::cerr << " * Connection attempt failed" << std::endl;
                    continue;
                }
    
                sendCommand(2, seq);
                
                MarketMessage message;
                if (readMessage(message)) {
                    logMessage(message);
                    recoveredCount++;
                    std::cout << " + Data recovered" << std::endl;
                }
                
                disconnectServer();
                delay_milliseconds(100);
            }
        }
    
        if (recoveredCount == missingCount) {
            std::cout << "\n+ COMPLETE: Successfully recovered all " << missingCount 
                     << " missing messages!" << std::endl;
        } else {
            std::cout << "\n! NOTICE: Recovered " << recoveredCount 
                     << " of " << missingCount << " missing messages." << std::endl;
        }
    
        std::cout << "\nData Recovery Results:" << std::endl;
        std::cout << "---------------------" << std::endl;
        std::cout << "Total Expected Sequences: " << maxSequence << std::endl;
        std::cout << "Missing Messages: " << missingCount << std::endl;
        std::cout << "Successfully Recovered: " << recoveredCount << std::endl;
        if (missingCount > 0) {
            std::cout << "Recovery Success Rate: " << (recoveredCount * 100.0 / missingCount) << "%" << std::endl;
        } else {
            std::cout << "Recovery Success Rate: 100%" << std::endl;
        }
    }
    
    void saveToFile() {
        std::cout << "[INFO] Writing data to 'output.json'..." << std::endl;
        
        std::ofstream outFile("output.json");
        outFile << "[\n";
        
        LoadingIndicator progress;
        size_t totalMessages = messageLog.size();
        
        for (size_t i = 0; i < totalMessages; ++i) {
            float completion = static_cast<float>(i + 1) / totalMessages;  
            progress.show(completion);
            
            bool isLast = (i == totalMessages - 1);
            outFile << messageToJSON(messageLog[i], isLast);
        }
        
        outFile << "]" << std::endl;
        outFile.close();
        
        progress.show(1.0);  
        std::cout << "\n[SUCCESS] Data export completed" << std::endl;
    }

public:
    MarketDataClient() : hostIP("127.0.0.1"), hostPort(3000) {
        #ifdef _WIN32
            int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
            if (result != 0) {
                std::cerr << "Network initialization failed: " << result << std::endl;
                exit(1);
            }
        #endif
    }

    ~MarketDataClient() {
        #ifdef _WIN32
            WSACleanup();
        #endif
    }

    void start() {
        sessionStart = std::chrono::steady_clock::now();
        logInitialization();
        
        std::cout << "-> Establishing connection to data source..." << std::endl;
        
        if (!openConnection()) {
            std::cerr << "* Connection failed - aborting" << std::endl;
            return;
        }
        
        std::cout << "-> Requesting initial data stream..." << std::endl;
        sendCommand(1);

        MarketMessage message;
        while (readMessage(message)) {
            logMessage(message);
        }

        disconnectServer();
        std::cout << "\n+ Initial data stream complete" << std::endl;

        int highestSequence = 0;
        for (const auto& msg : messageLog) {
            highestSequence = std::max(highestSequence, msg.sequenceNum);
        }

        fetchMissingData(highestSequence);

        std::cout << "-> Sorting messages by sequence number...";
        std::sort(messageLog.begin(), messageLog.end(), 
            [](const MarketMessage& a, const MarketMessage& b) {
                return a.sequenceNum < b.sequenceNum;
            });
        std::cout << " Done" << std::endl;

        saveToFile();
        generateReport();
        
        std::cout << "\n+ Process complete! Data saved to output.json\n" << std::endl;
    }
};

int main() {
    try {
        MarketDataClient client;
        client.start();
    } catch (const std::exception& e) {
        std::cerr << "Critical error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}