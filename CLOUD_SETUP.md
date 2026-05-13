# ESP-IDMS Cloud Setup Guide

Last updated: 2026-05-10

This guide is written for a first cloud setup. You do not need to understand
cloud infrastructure to start. The goal is simple:

1. ESP-IDMS measures current and temperature.
2. ESP-IDMS saves samples locally in `/spiffs/telemetry.csv`.
3. ESP-IDMS periodically sends new rows to a secure HTTPS URL.
4. A small cloud endpoint stores the rows so you can export and analyze them.

The firmware already has the device side implemented. You only need to create
an HTTPS endpoint and configure these serial console commands:

```text
set_cloud_url https://YOUR_ENDPOINT/ingest
set_cloud_token YOUR_RANDOM_SECRET
reboot
```

## Recommended Beginner Option

Use **Cloudflare Worker + Cloudflare KV** first.

Why this is recommended:

- It has a free plan suitable for early telemetry testing.
- It gives you a normal HTTPS URL.
- The ESP32 only stores one random token, not a database admin key.
- You do not need to run a server or keep a computer online.
- It matches the firmware's current `set_cloud_url` and `set_cloud_token`
  design.

Firebase is useful later for dashboards and mobile apps, but direct Firebase
REST writes need Firebase ID tokens or Google OAuth2 access tokens. Those are
not beginner-friendly on an ESP32, and long-lived service account keys should
not be stored in firmware.

## Words You Will See

| Word | Meaning |
|------|---------|
| Cloud | Someone else's server that is reachable through the internet |
| Endpoint | The URL that receives ESP-IDMS telemetry |
| Worker | Small JavaScript function running on Cloudflare |
| KV | Simple key/value storage, like a cloud folder of JSON files |
| Bearer token | A password sent in the HTTP `Authorization` header |
| JSON | Text format used to send structured data |

## What ESP-IDMS Sends

The cloud sync task sends JSON like this:

```json
{
  "schema": "esp-idms-csv-v1",
  "device_model": "ESP-IDMS",
  "serial_number": "B8F862E07D9C",
  "rows": [
    "1715320000,1.234,1,24.500,1,18.900,1,-5.600,1,0,0,0x00000000,1"
  ]
}
```

Each row is a CSV line with these columns:

```text
timestamp,current_a,current_valid,t_in_c,t_in_valid,t_out_c,t_out_valid,delta_t_c,delta_valid,power_fault,cooling_fault,sensor_flags,wifi_connected
```

The firmware uploads only new rows. The uploaded file offset is saved in NVS,
so failed uploads are retried later.

## Part 1: Create A Cloudflare Worker

1. Open `https://dash.cloudflare.com/`.
2. Create a free Cloudflare account, or sign in.
3. In the left menu, open **Workers & Pages**.
4. Select **Create application**.
5. Choose **Worker**.
6. Name it something like `esp-idms-ingest`.
7. Deploy the starter Worker.

At this point, Cloudflare gives you a URL similar to:

```text
https://esp-idms-ingest.YOUR-NAME.workers.dev
```

## Part 2: Create Storage

1. In Cloudflare, open **Storage & Databases**.
2. Open **Workers KV**.
3. Create a namespace named:

```text
IDMS_TELEMETRY
```

4. Go back to your Worker.
5. Open **Settings**.
6. Open **Bindings**.
7. Add a KV namespace binding:

```text
Variable name: IDMS_KV
KV namespace:  IDMS_TELEMETRY
```

## Part 3: Add A Secret Token

The token is your cloud password. Make it long and random.

Example shape:

```text
idms_9f3e2b7b6a1d4f4aa7f7e2c1b2a0d994
```

1. In the Worker settings, open **Variables and Secrets**.
2. Add a secret.
3. Name it:

```text
IDMS_TOKEN
```

4. Put your random token as the value.

Do not put this token in source code. In production, store it in encrypted NVS
on the ESP32.

## Part 4: Paste The Worker Code

Open the Worker editor and replace the starter code with this:

```js
export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method !== "POST" || url.pathname !== "/ingest") {
      return new Response("Not found", { status: 404 });
    }

    const expected = `Bearer ${env.IDMS_TOKEN}`;
    if (request.headers.get("Authorization") !== expected) {
      return new Response("Unauthorized", { status: 401 });
    }

    let payload;
    try {
      payload = await request.json();
    } catch {
      return new Response("Bad JSON", { status: 400 });
    }

    if (payload.schema !== "esp-idms-csv-v1" || !Array.isArray(payload.rows)) {
      return new Response("Invalid payload", { status: 400 });
    }

    const serial = String(payload.serial_number || "unknown")
      .replace(/[^A-Za-z0-9_-]/g, "_");
    const now = new Date();
    const key = `${serial}/${now.toISOString()}-${crypto.randomUUID()}.json`;

    await env.IDMS_KV.put(key, JSON.stringify({
      received_at: now.toISOString(),
      ...payload
    }));

    return Response.json({
      ok: true,
      stored_key: key,
      rows: payload.rows.length
    });
  }
};
```

Click **Save and deploy**.

## Part 4.5: Do Not Put Cloudflare Access In Front Of `/ingest`

Cloudflare Access is different from the ESP-IDMS Bearer token.

For this first setup, leave the Worker endpoint publicly reachable and let the
Worker code check `Authorization: Bearer ...`.

If your PowerShell test returns an HTML page with this title:

```text
Sign in - Cloudflare Access
```

then Cloudflare Access is blocking the request before it reaches the Worker.
The ESP32 cannot complete that browser login page.

Beginner fix:

1. Open Cloudflare dashboard.
2. Open **Zero Trust**.
3. Go to **Access controls > Applications**.
4. Find the application that protects your Worker hostname.
5. Remove that Access application, or remove the Worker hostname from it.
6. Keep the Worker `IDMS_TOKEN` secret as the protection for `/ingest`.

More advanced option:

- Protect browser dashboard/export pages with Cloudflare Access later.
- Leave `/ingest` reachable by the ESP32 and protected by the Worker Bearer
  token.
- Or use Cloudflare Access Service Auth, but that requires firmware support for
  `CF-Access-Client-ID` and `CF-Access-Client-Secret` headers. The current
  firmware does not send those headers.

## Part 5: Test From Your PC First

Before testing the ESP32, test the cloud endpoint from PowerShell:

```powershell
$token = "PASTE_YOUR_TOKEN_HERE"
$url = "https://YOUR_WORKER.YOUR_SUBDOMAIN.workers.dev/ingest"
$body = @{
  schema = "esp-idms-csv-v1"
  device_model = "ESP-IDMS"
  serial_number = "TEST_DEVICE"
  rows = @("1715320000,1.234,1,24.5,1,18.9,1,-5.6,1,0,0,0x00000000,1")
} | ConvertTo-Json

Invoke-RestMethod `
  -Method Post `
  -Uri $url `
  -Headers @{ Authorization = "Bearer $token" } `
  -ContentType "application/json" `
  -Body $body
```

Expected result:

```json
{
  "ok": true,
  "stored_key": "...",
  "rows": 1
}
```

Then open your KV namespace in Cloudflare and confirm a JSON entry was stored.

## Part 6: Configure ESP-IDMS

Open the ESP-IDMS serial console and run:

```text
set_cloud_url https://YOUR_WORKER.YOUR_SUBDOMAIN.workers.dev/ingest
set_cloud_token PASTE_YOUR_TOKEN_HERE
show_secrets
reboot
```

The cloud sync task starts after boot. It uploads pending telemetry every
`CONFIG_IDMS_CLOUD_UPLOAD_INTERVAL_S` seconds. The current default is 300
seconds.

Useful log lines:

```text
cloud: Cloud sync task started
cloud: Uploaded 16 telemetry row(s)
cloud: Cloud sync attempt failed
```

## Part 7: Export The Data

Beginner export method:

1. Open Cloudflare dashboard.
2. Open Workers KV.
3. Open `IDMS_TELEMETRY`.
4. Download or copy the JSON values.
5. Convert the `rows` array into CSV in Excel, Google Sheets, or a script.

Recommended next-phase improvement:

- Add a second Worker route, `/export`, that reads recent KV entries and returns
  one CSV file.
- Add a small dashboard page with charts for current, T_in, T_out, Delta T,
  power faults, cooling faults, and sensor faults.

## Firebase Option

Firebase Realtime Database can be used, but do not connect ESP-IDMS directly to
Firebase using a service account key.

Safe Firebase path:

1. ESP-IDMS sends telemetry to Cloudflare Worker.
2. Worker checks the Bearer token.
3. Worker stores in KV, or forwards to Firebase using server-side credentials.
4. Firebase is used for dashboard/app/reporting.

Why not direct Firebase from ESP32:

- Firebase REST auth uses Google OAuth2 access tokens or Firebase ID tokens.
- Service account private keys are powerful and long-lived.
- Storing them on a microcontroller is not safe.
- Firebase Cloud Functions are on the Blaze/pay-as-you-go plan, not the no-card
  Spark plan.

## Security Checklist Before Production

- [ ] Rotate all test tokens.
- [ ] Use a long random `set_cloud_token`.
- [ ] Use HTTPS only.
- [ ] Enable production secure boot, flash encryption, and NVS encryption.
- [ ] Do not compile Wi-Fi, Telegram, OTA, or cloud secrets into firmware.
- [ ] Keep Cloudflare Worker token secret.
- [ ] Rotate any token that was copied into chat, screenshots, tickets, or logs.
- [ ] Use one token per customer/site if possible.
- [ ] Add an export/backup process before relying on cloud history.

## Troubleshooting

| Symptom | Check |
|---------|-------|
| HTML page titled `Sign in - Cloudflare Access` | Cloudflare Access is protecting the Worker. Remove Access from `/ingest`, or add firmware support for Access Service Auth headers |
| `401 Unauthorized` | Token in ESP-IDMS must match Worker `IDMS_TOKEN` |
| `404 Not found` | URL must end with `/ingest` |
| `Cloud sync disabled: no URL configured` | Run `set_cloud_url ...` |
| `Cloud sync attempt failed` | Check Wi-Fi, DNS, token, Worker logs |
| No KV rows | Confirm KV binding variable name is exactly `IDMS_KV` |
| Uploads only every few minutes | Default interval is 300 seconds |

## Official References

- Cloudflare Workers limits: https://developers.cloudflare.com/workers/platform/limits/
- Cloudflare Workers pricing: https://developers.cloudflare.com/workers/platform/pricing/
- Cloudflare KV pricing: https://developers.cloudflare.com/kv/platform/pricing/
- Cloudflare Access policies: https://developers.cloudflare.com/cloudflare-one/access-controls/policies/
- Cloudflare Access service tokens: https://developers.cloudflare.com/api/resources/zero_trust/subresources/access/subresources/service_tokens/
- Firebase pricing: https://firebase.google.com/pricing
- Firebase Realtime Database REST auth: https://firebase.google.com/docs/database/rest/auth
