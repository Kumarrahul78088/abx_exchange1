# ABX Exchange Client

## System Requirements
- Node.js runtime (v16.17.0+)
- Any modern C++ compiler

## Getting Started

### Download Source Code
Retrieve the project repository:
```
git clone https://github.com/Kumarrahul78088/abx_exchange1.git
```

### Server Configuration
1. Access the server folder:
```
cd abx_exchange_server
```

2. Launch the TCP server:
```
node main.js
```

3. Verify connection with message:
```
TCP server started on port 3000
```

### Client Setup
1. Launch a separate terminal instance
2. Navigate to client directory:
```
cd abx_exchange_client
```

3. Build the client application:
```
g++ -std=c++11 abx_client.cpp -o abx_client -lws2_32
```

4. Execute the compiled program:
```
abx_client
```

## Data Output
The client automatically generates a JSON data file (`output.json`) containing all market data packets received from the server. Review this file to analyze the collected stock market information.
