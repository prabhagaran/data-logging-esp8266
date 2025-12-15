# Google Apps Script Backend

```javascript
function doGet(e) {
  const API_KEY = "abc123";
  if (!e.parameter.key || e.parameter.key !== API_KEY) return ContentService.createTextOutput("Unauthorized");
  if (!e.parameter.temp || !e.parameter.hum || !e.parameter.volt) return ContentService.createTextOutput("Missing parameters");
  const sheet = SpreadsheetApp.getActiveSpreadsheet().getSheetByName("Sheet1");
  sheet.appendRow([new Date(), e.parameter.id || "UNKNOWN", Number(e.parameter.temp), Number(e.parameter.hum), Number(e.parameter.volt)]);
  return ContentService.createTextOutput("OK");
}
```
