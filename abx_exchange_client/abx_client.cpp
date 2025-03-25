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

// Constants
const char* DEFAULT_HOST_IP = "127.0.0.1";
const int DEFAULT_HOST_PORT = 3000;
const int LOADING_BAR_WIDTH = 50;

// Enums for commands and error types
enum class CommandType : uint8_t {
    INITIAL_STREAM = 1,
    SPECIFIC_SEQUENCE = 2
};

enum class NetworkErrorType {
    SOCKET_CREATION,
    CONNECTION,
    DATA_RECEPTION
};

// Data structure for message format
struct MarketMessage {
    char assetCode[5];
    char orderDirection;
    int32_t size;
    int32_t cost;
    int32_t sequenceNum;
};

// Utility Functions
namespace Utilities {
    void printError(NetworkErrorType type, int errorCode = 0) {
        switch (type) {
            case NetworkErrorType::SOCKET_CREATION:
                std::cerr << "Socket creation error: " << errorCode << std::endl;
                break;
            case NetworkErrorType::CONNECTION:
                std::cerr << "Connection failed: " << errorCode << std::endl;
                break;
            case NetworkErrorType::DATA_RECEPTION:
                std::cerr << "Data reception error: " << errorCode << std::endl;
                break;
        }
    }

    std::string generateErrorMessage(const std::string& context, int errorCode) {
        std::stringstream ss;
        ss << context << " Error Code: " << errorCode;
        return ss.str();
    }
}

// Visual feedback component
class LoadingIndicator {
private:
    int barWidth;
    float percentComplete;

public:
    explicit LoadingIndicator(int width = LOADING_BAR_WIDTH) 
        : barWidth(width), percentComplete(0) {}

    void show(float percent) {
        percentComplete = std::min(1.0f, std::max(0.0f, percent));
        int filledWidth = static_cast<int>(barWidth * percentComplete);
        
        std::cout << "\r[";
        for (int i = 0; i < barWidth; ++i) {
            if (i < filledWidth) std::cout << "=";
            else if (i == filledWidth) std::cout << ">";
            else std::cout << " ";
        }
        std::cout << "] " << static_cast<int>(percentComplete * 100.0) << "%" << std::flush;
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

    // Network Initialization
    bool initializeNetworkStack() {
        #ifdef _WIN32
            int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
            if (result != 0) {
                Utilities::printError(NetworkErrorType::SOCKET_CREATION, result);
                return false;
            }
        #endif
        return true;
    }

    void cleanupNetworkStack() {
        #ifdef _WIN32
            WSACleanup();
        #endif
    }

    // Connection Management
    bool createSocket() {
        #ifdef _WIN32
            socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socketHandle == INVALID_SOCKET) {
                Utilities::printError(NetworkErrorType::SOCKET_CREATION, WSAGetLastError());
                return false;
            }
        #else
            socketHandle = socket(AF_INET, SOCK_STREAM, 0);
            if (socketHandle < 0) {
                Utilities::printError(NetworkErrorType::SOCKET_CREATION);
                return false;
            }
        #endif
        return true;
    }

    bool connectToServer(const char* ip, int port) {
        if (!createSocket()) return false;

        struct sockaddr_in serverAddress;
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = htons(port);
        serverAddress.sin_addr.s_addr = inet_addr(ip);

        if (connect(socketHandle, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
            #ifdef _WIN32
                Utilities::printError(NetworkErrorType::CONNECTION, WSAGetLastError());
                closesocket(socketHandle);
            #else
                Utilities::printError(NetworkErrorType::CONNECTION);
                close(socketHandle);
            #endif
            return false;
        }

        std::cout << "[SUCCESS] Connected to data server" << std::endl;
        return true;
    }

    void disconnectServer() {
        #ifdef _WIN32
            closesocket(socketHandle);
        #else
            close(socketHandle);
        #endif
    }

    // Data Transmission and Reception
    bool sendCommand(CommandType commandCode, uint8_t sequenceParam = 0) {
        uint8_t commandBuffer[2] = {
            static_cast<uint8_t>(commandCode), 
            sequenceParam
        };
        
        return send(socketHandle, 
            reinterpret_cast<const char*>(commandBuffer), 
            sizeof(commandBuffer), 0) >= 0;
    }

    bool receiveMessage(MarketMessage& message) {
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
                if (bytesReceived == 0) return false; // Connection closed
                
                #ifdef _WIN32
                    if (WSAGetLastError() == WSAEINTR) continue;
                    Utilities::printError(NetworkErrorType::DATA_RECEPTION, WSAGetLastError());
                #else
                    if (errno == EINTR) continue;
                    Utilities::printError(NetworkErrorType::DATA_RECEPTION, errno);
                #endif
                return false;
            }
            totalBytesReceived += static_cast<size_t>(bytesReceived);
        }
    
        // Parse buffer into MarketMessage
        memcpy(message.assetCode, buffer, 4);
        message.assetCode[4] = '\0';
        message.orderDirection = buffer[4];
        message.size = ntohl(*reinterpret_cast<int32_t*>(buffer + 5));
        message.cost = ntohl(*reinterpret_cast<int32_t*>(buffer + 9));
        message.sequenceNum = ntohl(*reinterpret_cast<int32_t*>(buffer + 13));
        
        return true;
    }

    // Logging and Reporting
    void logMessage(const MarketMessage& message) {
        messageLog.push_back(message);
        processedSequences.insert(message.sequenceNum);
        std::cout << "[RECEIVED] Message " << message.sequenceNum 
                  << " (" << message.assetCode << ")" << std::endl;
    }

    void generateSessionReport() {
        auto currentTime = std::chrono::steady_clock::now();
        auto totalRuntime = std::chrono::duration_cast<std::chrono::seconds>(
            currentTime - sessionStart).count();
    
        std::cout << "\n[INFO] Session Report" << std::endl;
        std::cout << "-----------------------------------" << std::endl;
        std::cout << "Total Messages       : " << messageLog.size() << std::endl;
        std::cout << "Session Duration     : " << totalRuntime << "s" << std::endl;
        std::cout << "Processing Rate      : " 
                  << messageLog.size() / (totalRuntime ? totalRuntime : 1) 
                  << " msg/s" << std::endl;
    }

    // Data Recovery
    void recoverMissingData(int maxSequence) {
        std::cout << "\n-> Validating data integrity..." << std::endl;
        
        LoadingIndicator progress;
        int missingCount = 0, recoveredCount = 0;
        
        for (int seq = 1; seq <= maxSequence; ++seq) {
            progress.show(float(seq) / maxSequence);
            
            if (processedSequences.find(seq) == processedSequences.end()) {
                missingCount++;
                std::cout << "\n! Requesting sequence number: " << seq;
                
                if (!connectToServer(hostIP, hostPort)) {
                    std::cerr << " * Connection attempt failed" << std::endl;
                    continue;
                }
    
                sendCommand(CommandType::SPECIFIC_SEQUENCE, seq);
                
                MarketMessage message;
                if (receiveMessage(message)) {
                    logMessage(message);
                    recoveredCount++;
                    std::cout << " + Data recovered" << std::endl;
                }
                
                disconnectServer();
                delay_milliseconds(100);
            }
        }
    
        printRecoveryResults(missingCount, recoveredCount, maxSequence);
    }

    void printRecoveryResults(int missingCount, int recoveredCount, int maxSequence) {
        if (recoveredCount == missingCount) {
            std::cout << "\n+ COMPLETE: Successfully recovered all " 
                      << missingCount << " missing messages!" << std::endl;
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
            std::cout << "Recovery Success Rate: " 
                      << (recoveredCount * 100.0 / missingCount) << "%" << std::endl;
        } else {
            std::cout << "Recovery Success Rate: 100%" << std::endl;
        }
    }

    // File Export
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

    void exportToJSONFile() {
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
    // Constructor and Destructor
    MarketDataClient(
        const char* ip = DEFAULT_HOST_IP, 
        int port = DEFAULT_HOST_PORT
    ) : hostIP(ip), hostPort(port) {
        if (!initializeNetworkStack()) {
            throw std::runtime_error("Network stack initialization failed");
        }
    }

    ~MarketDataClient() {
        cleanupNetworkStack();
    }

    // Main process method
    void start() {
        sessionStart = std::chrono::steady_clock::now();
        
        // Initial Connection and Data Stream
        if (!connectToServer(hostIP, hostPort)) {
            std::cerr << "* Initial connection failed - aborting" << std::endl;
            return;
        }
        
        std::cout << "-> Requesting initial data stream..." << std::endl;
        sendCommand(CommandType::INITIAL_STREAM);

        // Receive Messages
        MarketMessage message;
        while (receiveMessage(message)) {
            logMessage(message);
        }

        disconnectServer();
        std::cout << "\n+ Initial data stream complete" << std::endl;

        // Find Highest Sequence Number
        int highestSequence = findHighestSequenceNumber();

        // Recover Missing Data
        recoverMissingData(highestSequence);

        // Sort Messages
        sortMessagesBySequence();

        // Export Data
        exportToJSONFile();
        generateSessionReport();
        
        std::cout << "\n+ Process complete! Data saved to output.json\n" << std::endl;
    }

private:
    // Helper Methods
    int findHighestSequenceNumber() {
        std::cout << "-> Sorting messages by sequence number...";
        int highestSequence = 0;
        for (const auto& msg : messageLog) {
            highestSequence = std::max(highestSequence, msg.sequenceNum);
        }
        std::cout << " Done" << std::endl;
        return highestSequence;
    }

    void sortMessagesBySequence() {
        std::sort(messageLog.begin(), messageLog.end(), 
            [](const MarketMessage& a, const MarketMessage& b) {
                return a.sequenceNum < b.sequenceNum;
            });
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
