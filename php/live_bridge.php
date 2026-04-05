<?php
declare(strict_types=1);

$cliOptions = getopt('', [
    'lat:',
    'lon:',
    'title:',
    'location:',
    'interval:',
    'temp-unit:',
    'once',
    'mode:',
    'out:',
    'ultimate-ip:',
    'ultimate-max-tries:',
    'address:',
    'password:',
]);
$bridgeDefaults = [
    'title' => 'api.open-meteo.com',
    'location' => 'New York',
    'lat' => '40.7128',
    'lon' => '-74.0060',
    'interval' => '900',
    'temp_unit' => 'C',
    'address' => 'C800',
    'ultimate_max_tries' => '3',
    'mode' => 'file',
    'out' => dirname(__DIR__) . DIRECTORY_SEPARATOR . 'LIVE.DAT',
];

function option_value(array $cliOptions, string $name, string $default): string
{
    if (isset($cliOptions[$name]) && $cliOptions[$name] !== false) {
        return (string) $cliOptions[$name];
    }
    return $default;
}

function log_message(string $message): void
{
    echo '[' . date('Y-m-d H:i:s') . '] ' . $message . PHP_EOL;
}

function http_request(string $url, string $method = 'GET', array $headers = []): string
{
    $headers[] = 'User-Agent: c64-live-dashboard/1.0';

    if (function_exists('curl_init')) {
        $ch = curl_init($url);
        curl_setopt_array($ch, [
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_FOLLOWLOCATION => true,
            CURLOPT_TIMEOUT => 15,
            CURLOPT_CUSTOMREQUEST => $method,
            CURLOPT_HTTPHEADER => $headers,
        ]);

        $body = curl_exec($ch);
        $code = (int) curl_getinfo($ch, CURLINFO_RESPONSE_CODE);

        if (PHP_VERSION_ID < 80000) {
            curl_close($ch);
        }

        if ($body !== false && $code < 400) {
            return (string) $body;
        }
    }

    $contextHeader = implode("\r\n", $headers) . "\r\n";
    $context = stream_context_create([
        'http' => [
            'method' => $method,
            'timeout' => 15,
            'header' => $contextHeader,
            'ignore_errors' => true,
        ],
    ]);

    $body = @file_get_contents($url, false, $context);
    if ($body !== false) {
        return $body;
    }

    if (function_exists('exec')) {
        $command = 'curl.exe --fail --silent --show-error -X ' . escapeshellarg($method);
        foreach ($headers as $header) {
            $command .= ' -H ' . escapeshellarg($header);
        }
        $command .= ' ' . escapeshellarg($url) . ' 2>&1';

        $output = [];
        $exitCode = 1;
        exec($command, $output, $exitCode);

        if ($exitCode === 0) {
            return implode(PHP_EOL, $output);
        }
    }

    throw new RuntimeException('Unable to complete HTTP request: ' . $method . ' ' . $url);
}

function clamp_value(int $value, int $min, int $max): int
{
    if ($value < $min) {
        return $min;
    }

    if ($value > $max) {
        return $max;
    }

    return $value;
}

function normalize_temp_unit(string $tempUnit): string
{
    return strtoupper(substr(trim($tempUnit), 0, 1)) === 'F' ? 'F' : 'C';
}

function fit_field(string $value, int $length): string
{
    $value = strtoupper($value);
    $value = preg_replace('/[^A-Z0-9 .:\-\/]/', ' ', $value) ?? $value;
    $value = substr($value, 0, $length);

    return str_pad($value, $length, ' ');
}

function map_condition(int $code): string
{
    switch ($code) {
        case 0:
            return 'CLEAR';
        case 1:
        case 2:
            return 'PARTLY CLOUDY';
        case 3:
            return 'CLOUDY';
        case 45:
        case 48:
            return 'FOG';
        case 51:
        case 53:
        case 55:
        case 56:
        case 57:
        case 61:
        case 63:
        case 65:
        case 66:
        case 67:
        case 80:
        case 81:
        case 82:
            return 'RAIN';
        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86:
            return 'SNOW';
        case 95:
        case 96:
        case 99:
            return 'STORM';
        default:
            return 'MIXED';
    }
}

function map_icon_code(int $code): int
{
    switch ($code) {
        case 0:
            return 0;
        case 1:
        case 2:
        case 3:
            return 1;
        case 45:
        case 48:
            return 4;
        case 51:
        case 53:
        case 55:
        case 56:
        case 57:
        case 61:
        case 63:
        case 65:
        case 66:
        case 67:
        case 80:
        case 81:
        case 82:
            return 2;
        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86:
            return 5;
        case 95:
        case 96:
        case 99:
            return 3;
        default:
            return 1;
    }
}

function write_live_file(string $path, string $contents): void
{
    $dir = dirname($path);
    if (!is_dir($dir)) {
        mkdir($dir, 0777, true);
    }

    $tmp = $path . '.tmp';
    if (file_put_contents($tmp, $contents, LOCK_EX) === false) {
        throw new RuntimeException('Unable to write temp file: ' . $tmp);
    }

    if (!rename($tmp, $path)) {
        throw new RuntimeException('Unable to rename temp file into place: ' . $path);
    }
}

function build_payload_text(string $title, string $location, array $current, string $tempUnit): string
{
    $tempUnit = normalize_temp_unit($tempUnit);
    $condition = map_condition((int) ($current['weather_code'] ?? 0));
    $temp = (int) round((float) ($current['temperature_2m'] ?? 0));
    $humidity = clamp_value((int) round((float) ($current['relative_humidity_2m'] ?? 0)), 0, 100);
    $wind = clamp_value((int) round((float) ($current['wind_speed_10m'] ?? 0)), 0, 255);
    $rain = clamp_value((int) round((float) ($current['precipitation'] ?? 0)), 0, 255);
    $stamp = strtoupper(substr((string) ($current['time'] ?? date('Y-m-d\TH:i')), 0, 16));

    $lines = [
        'TITLE=' . strtoupper(substr($title, 0, 20)),
        'LOCATION=' . strtoupper(substr($location, 0, 20)),
        'CONDITION=' . strtoupper(substr($condition, 0, 16)),
        'TEMP_' . $tempUnit . '=' . (string) $temp,
        'TEMP_UNIT=' . $tempUnit,
        'HUMIDITY=' . (string) $humidity,
        'WIND_KPH=' . (string) $wind,
        'RAIN_MM=' . (string) $rain,
        'STAMP=' . $stamp,
    ];

    return implode(PHP_EOL, $lines) . PHP_EOL;
}

function build_memory_packet(string $title, string $location, array $current, string $tempUnit, int $interval): string
{
    static $sequence = null;

    if ($sequence === null) {
        $sequence = (int) (time() & 0xFF);
    } else {
        $sequence = ($sequence + 1) & 0xFF;
    }

    $tempUnit = normalize_temp_unit($tempUnit);
    $weatherCode = (int) ($current['weather_code'] ?? 0);
    $temp = clamp_value((int) round((float) ($current['temperature_2m'] ?? 0)), -99, 155);
    $humidity = clamp_value((int) round((float) ($current['relative_humidity_2m'] ?? 0)), 0, 100);
    $wind = clamp_value((int) round((float) ($current['wind_speed_10m'] ?? 0)), 0, 255);
    $rain = clamp_value((int) round((float) ($current['precipitation'] ?? 0)), 0, 255);
    $condition = map_condition($weatherCode);
    $stamp = substr((string) ($current['time'] ?? date('Y-m-d\TH:i')), 0, 16);
    $pollIntervalUnitSeconds = 15;
    $pollIntervalSeconds = clamp_value($interval, 0, $pollIntervalUnitSeconds * 127);
    $pollIntervalEncoded = $pollIntervalSeconds > 0 ? (int) ceil($pollIntervalSeconds / $pollIntervalUnitSeconds) : 0;
    $packetFlags = (($pollIntervalEncoded & 0x7F) << 1) | ($tempUnit === 'F' ? 1 : 0);

    $packet = pack(
        'C*',
        0xA5,
        $sequence,
        $temp + 100,
        $humidity,
        $wind,
        $rain,
        map_icon_code($weatherCode),
        $packetFlags
    );

    $packet .= fit_field($title, 20);
    $packet .= fit_field($location, 20);
    $packet .= fit_field($condition, 16);
    $packet .= fit_field($stamp, 16);

    return $packet;
}

function send_to_ultimate_memory(string $ultimateIp, string $address, string $packet, string $password): array
{
    $cleanAddress = strtoupper(preg_replace('/[^0-9A-F]/i', '', $address) ?? '');
    if ($cleanAddress === '') {
        $cleanAddress = 'C800';
    }

    $hexData = strtoupper(bin2hex($packet));
    $url = sprintf(
        'http://%s/v1/machine:writemem?address=%s&data=%s',
        $ultimateIp,
        $cleanAddress,
        $hexData
    );

    $headers = [];
    if ($password !== '') {
        $headers[] = 'X-Password: ' . $password;
    }

    $response = http_request($url, 'PUT', $headers);
    $trimmedResponse = trim($response);
    $decodedResponse = json_decode($trimmedResponse, true);

    if (is_array($decodedResponse)
        && isset($decodedResponse['errors'])
        && is_array($decodedResponse['errors'])
        && count($decodedResponse['errors']) > 0) {
        throw new RuntimeException('Ultimate API returned errors: ' . implode('; ', $decodedResponse['errors']));
    }

    return [
        'url' => 'http://' . $ultimateIp . '/v1/machine:writemem?address=' . $cleanAddress . '&data=<hex ' . strlen($packet) . ' bytes>',
        'address' => $cleanAddress,
        'bytes' => strlen($packet),
        'response' => $trimmedResponse,
    ];
}

function fetch_live_current(float $lat, float $lon, string $tempUnit): array
{
    $apiTempUnit = normalize_temp_unit($tempUnit) === 'F' ? 'fahrenheit' : 'celsius';
    $url = sprintf(
        'https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&current=temperature_2m,relative_humidity_2m,wind_speed_10m,precipitation,weather_code&wind_speed_unit=kmh&temperature_unit=%s&timezone=auto',
        rawurlencode((string) $lat),
        rawurlencode((string) $lon),
        rawurlencode($apiTempUnit)
    );

    $json = http_request($url, 'GET');
    $data = json_decode($json, true);

    if (!is_array($data) || !isset($data['current']) || !is_array($data['current'])) {
        throw new RuntimeException('Unexpected API response');
    }

    return $data['current'];
}

$title = option_value($cliOptions, 'title', $bridgeDefaults['title']);
$lat = (float) option_value($cliOptions, 'lat', $bridgeDefaults['lat']);
$lon = (float) option_value($cliOptions, 'lon', $bridgeDefaults['lon']);
$location = option_value($cliOptions, 'location', $bridgeDefaults['location']);
$interval = max(15, (int) option_value($cliOptions, 'interval', $bridgeDefaults['interval']));
$tempUnit = normalize_temp_unit(option_value($cliOptions, 'temp-unit', $bridgeDefaults['temp_unit']));
$ultimateIp = option_value($cliOptions, 'ultimate-ip', '');
$ultimateMaxTries = max(1, (int) option_value($cliOptions, 'ultimate-max-tries', $bridgeDefaults['ultimate_max_tries']));
$address = option_value($cliOptions, 'address', $bridgeDefaults['address']);
$password = option_value($cliOptions, 'password', '');
$outFile = option_value($cliOptions, 'out', $bridgeDefaults['out']);
$outFile = str_replace(['/', '\\'], DIRECTORY_SEPARATOR, $outFile);
$defaultMode = $ultimateIp !== '' ? 'mem' : $bridgeDefaults['mode'];
$mode = strtolower(option_value($cliOptions, 'mode', $defaultMode));
$once = isset($cliOptions['once']);

if ($mode === 'mem' && $ultimateIp === '') {
    throw new InvalidArgumentException('mem mode requires --ultimate-ip');
}

log_message('mode: ' . $mode);
if ($mode === 'mem') {
    log_message('target: ' . $ultimateIp . ' at $' . strtoupper($address));
} else {
    log_message('output file: ' . $outFile);
}

$consecutiveUltimateFailures = 0;
$exitCode = 0;
$stopRequested = false;

do {
try {
    $current = fetch_live_current($lat, $lon, $tempUnit);
    $payload = build_payload_text($title, $location, $current, $tempUnit);

    if ($mode === 'mem') {
        try {
            $packet = build_memory_packet($title, $location, $current, $tempUnit, $interval);
            $pushInfo = send_to_ultimate_memory($ultimateIp, $address, $packet, $password);
            $consecutiveUltimateFailures = 0;
            log_message('pushed ' . (string) $pushInfo['bytes'] . ' bytes');
        } catch (Throwable $e) {
            $consecutiveUltimateFailures++;
            $exitCode = 1;
            fwrite(STDERR, '[' . date('Y-m-d H:i:s') . '] c64u push failed (' . $consecutiveUltimateFailures . '/' . $ultimateMaxTries . '): ' . $e->getMessage() . PHP_EOL);

            if ($consecutiveUltimateFailures >= $ultimateMaxTries) {
                fwrite(STDERR, '[' . date('Y-m-d H:i:s') . '] c64u failure limit reached; terminating.' . PHP_EOL);
                $stopRequested = true;
            }
        }
    } else {
        write_live_file($outFile, $payload);
        log_message('wrote to ' . $outFile);
    }

    echo $payload . PHP_EOL;
} catch (Throwable $e) {
    $exitCode = 1;
    fwrite(STDERR, '[' . date('Y-m-d H:i:s') . '] error: ' . $e->getMessage() . PHP_EOL);
}

if ($once || $stopRequested) {
    break;
}

sleep($interval);
} while (true);

exit($exitCode);
