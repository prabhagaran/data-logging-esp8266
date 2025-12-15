# System Architecture

## Overview

This project implements a **serverless IoT data logging architecture** where a
**NodeMCU (ESP8266)** sends sensor data to **Google Sheets** using
**Google Apps Script** as an HTTP backend.

The design avoids dedicated servers and databases, making it simple,
cost-effective, and scalable for prototyping and small-scale deployments.

---

## High-Level Architecture
```
plantuml
@startuml
title High-Level Architecture

actor "NodeMCU (ESP8266)" as MCU

rectangle "Google Apps Script\n(Web App API)" as GAS {
  component "Auth Check"
  component "Validation"
  component "Parsing"
}

database "Google Sheets" as SHEET

MCU --> GAS : HTTPS GET Request\n(JSON via URL params)
GAS --> SHEET : appendRow()

note right of MCU
Sensor Data
- Temperature
- Humidity
- Voltage
end note

note right of SHEET
- Timestamped
- Structured
- Persistent
end note

@enduml
```
## Data Flow Description

1. **Data Acquisition**
   - NodeMCU reads sensor values such as:
     - Temperature
     - Humidity
     - Battery Voltage

2. **HTTP Transmission**
   - NodeMCU sends data using an HTTPS `GET` request
   - Data is encoded as URL query parameters

3. **Backend Processing**
   - Google Apps Script receives the request
   - Validates API key and required parameters
   - Converts values to numeric types

4. **Data Storage**
   - Valid data is appended as a new row in Google Sheets
   - Each entry is timestamped automatically

---

## Components Description

### NodeMCU (ESP8266)
- Acts as the data source
- Connects to Wi-Fi
- Sends HTTPS requests
- Low power and low cost

### Google Apps Script
- Serverless HTTP endpoint
- Runs on Google infrastructure
- Handles authentication and validation
- Writes data to Google Sheets

### Google Sheets
- Acts as a lightweight database
- Stores structured time-series data
- Supports visualization and sharing

---

## Communication Protocol

| Parameter | Value |
|--------|-------|
| Protocol | HTTPS |
| Method | GET |
| Payload | URL query parameters |
| Security | API key validation |
| Response | Plain text (`OK`, error message) |

---

## Design Considerations

- **Serverless**: No maintenance of backend servers
- **Scalable**: Multiple NodeMCUs can write to the same sheet
- **Secure (Basic)**: API key-based access control
- **Extensible**: Easy to add new parameters or devices

---

## Limitations (Current Architecture)

- Uses HTTP GET instead of POST
- No rate limiting or retry mechanism
- API key is hardcoded
- Google Apps Script execution quotas apply

---

## Future Improvements

- Switch to HTTP POST with JSON payload
- Add authentication tokens per device
- Implement error logging and retries
- Introduce daily or device-wise sheets
- Add cloud database for large-scale deployments

---

## Summary

This architecture provides a **simple, robust, and maintainable**
solution for IoT data logging using widely available tools,
making it ideal for rapid development and proof-of-concept systems.
