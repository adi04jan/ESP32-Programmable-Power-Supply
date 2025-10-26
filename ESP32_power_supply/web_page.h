const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Power Supply Control</title>
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }

        body {
            font-family: Arial, sans-serif;
            background: #1a1a1a;
            color: #e0e0e0;
            padding: 10px;
            min-height: 100vh;
        }

        .container {
            max-width: 800px;
            margin: 0 auto;
            padding: 15px;
        }

        h1 {
            text-align: center;
            font-size: 1.5rem;
            margin-bottom: 20px;
            color: #4CAF50;
        }

        .output-card {
            background: #2a2a2a;
            border-radius: 8px;
            padding: 15px;
            margin-bottom: 15px;
            border: 1px solid #3a3a3a;
        }

        .output-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 15px;
        }

        .output-title {
            font-size: 1.1rem;
            font-weight: bold;
        }

        .status-indicator {
            width: 12px;
            height: 12px;
            border-radius: 50%;
            background: #666;
            transition: background 0.3s;
        }

        .status-indicator.on {
            background: #4CAF50;
            box-shadow: 0 0 8px #4CAF50;
        }

        .display {
            background: #1a1a1a;
            border: 2px solid #3a3a3a;
            border-radius: 4px;
            padding: 12px;
            text-align: center;
            font-size: 1.8rem;
            font-weight: bold;
            color: #4CAF50;
            margin-bottom: 15px;
            font-family: 'Courier New', monospace;
        }

        .slider-container {
            margin-bottom: 15px;
        }

        .slider {
            width: 100%;
            height: 8px;
            border-radius: 4px;
            background: #3a3a3a;
            outline: none;
            -webkit-appearance: none;
        }

        .slider::-webkit-slider-thumb {
            -webkit-appearance: none;
            appearance: none;
            width: 20px;
            height: 20px;
            border-radius: 50%;
            background: #4CAF50;
            cursor: pointer;
        }

        .slider::-moz-range-thumb {
            width: 20px;
            height: 20px;
            border-radius: 50%;
            background: #4CAF50;
            cursor: pointer;
            border: none;
        }

        .toggle-btn {
            width: 100%;
            padding: 12px;
            border: none;
            border-radius: 6px;
            font-size: 1rem;
            font-weight: bold;
            cursor: pointer;
            transition: all 0.3s;
            text-transform: uppercase;
        }

        .toggle-btn.off {
            background: #555;
            color: #aaa;
        }

        .toggle-btn.on {
            background: #4CAF50;
            color: white;
        }

        .toggle-btn:active {
            transform: scale(0.98);
        }

        .modal {
            display: none;
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background: rgba(0, 0, 0, 0.8);
            z-index: 1000;
            justify-content: center;
            align-items: center;
        }

        .modal.show {
            display: flex;
        }

        .modal-content {
            background: #2a2a2a;
            border: 2px solid #ff9800;
            border-radius: 8px;
            padding: 20px;
            max-width: 400px;
            margin: 20px;
        }

        .modal-header {
            color: #ff9800;
            font-size: 1.3rem;
            font-weight: bold;
            margin-bottom: 15px;
            display: flex;
            align-items: center;
            gap: 10px;
        }

        .modal-body {
            margin-bottom: 20px;
            line-height: 1.6;
        }

        .modal-checkbox {
            display: flex;
            align-items: center;
            gap: 8px;
            margin-bottom: 15px;
            cursor: pointer;
        }

        .modal-checkbox input {
            width: 18px;
            height: 18px;
            cursor: pointer;
        }

        .modal-buttons {
            display: flex;
            gap: 10px;
        }

        .modal-btn {
            flex: 1;
            padding: 10px;
            border: none;
            border-radius: 6px;
            font-weight: bold;
            cursor: pointer;
            transition: all 0.3s;
        }

        .modal-btn.proceed {
            background: #ff9800;
            color: white;
        }

        .modal-btn.cancel {
            background: #555;
            color: #aaa;
        }

        .modal-btn:hover {
            opacity: 0.9;
        }

        .about-btn {
            position: fixed;
            bottom: 20px;
            right: 20px;
            padding: 8px 16px;
            background: #3a3a3a;
            color: #4CAF50;
            border: 1px solid #4CAF50;
            border-radius: 6px;
            cursor: pointer;
            font-size: 0.9rem;
            transition: all 0.3s;
            z-index: 999;
        }

        .about-btn:hover {
            background: #4CAF50;
            color: white;
        }

        .about-content {
            text-align: center;
        }

        .about-content h2 {
            color: #4CAF50;
            margin-bottom: 15px;
        }

        .about-content p {
            margin: 10px 0;
            line-height: 1.8;
        }

        .about-content a {
            color: #4CAF50;
            text-decoration: none;
            font-weight: bold;
        }

        .about-content a:hover {
            text-decoration: underline;
        }

        .version {
            color: #999;
            font-size: 0.9rem;
        }

        @media (min-width: 600px) {
            h1 {
                font-size: 2rem;
            }
            
            .output-card {
                padding: 20px;
            }
        }
    </style>
</head>
<body>
    <!-- Warning Modal -->
    <div class="modal" id="warningModal">
        <div class="modal-content">
            <div class="modal-header">⚠️ High Voltage Warning</div>
            <div class="modal-body">
                You are about to enable output with voltage above 5V. Please ensure your device can handle this voltage level.
            </div>
            <label class="modal-checkbox">
                <input type="checkbox" id="dontShowAgain">
                <span>Don't show this warning again</span>
            </label>
            <div class="modal-buttons">
                <button class="modal-btn cancel" onclick="cancelWarning()">Cancel</button>
                <button class="modal-btn proceed" onclick="proceedWithToggle()">Proceed</button>
            </div>
        </div>
    </div>
    <!-- About Modal -->
    <div class="modal" id="aboutModal">
        <div class="modal-content">
            <div class="about-content">
                <h2>⚡ Power Supply Control</h2>
                <p class="version">Version 1.0.0</p>
                <p><strong>Developer:</strong> Aditya Biswas</p>
                <p><strong>Release Date:</strong> October 25, 2025</p>
                <p style="margin-top: 15px;">
                    <a href="https://github.com/adi04jan" target="_blank" rel="noopener noreferrer">👤 View Profile</a>
                </p>
            </div>
            <div class="modal-buttons" style="margin-top: 20px;">
                <button class="modal-btn cancel" onclick="closeAbout()" style="flex: none; width: 100%;">Close</button>
            </div>
        </div>
    </div>
    <div class="container">
        <h1>⚡ Power Supply Control</h1>
        <!-- Output 1: Variable with Voltage Control and Quick Set Buttons -->
        <div class="output-card">
            <div class="output-header">
                <span class="output-title">Output 1 (Variable)</span>
                <div class="status-indicator" id="status1"></div>
            </div>
            <div class="display" id="display1">0.0V</div>
            <div class="slider-container">
                <input type="range" min="2.5" max="15" step="0.1" value="2.5" class="slider" id="voltage1">
                <!-- Quick Set Buttons Start -->
                <div style="display:flex; gap:8px; margin-top:10px;">
                    <button class="toggle-btn" style="padding:6px 10px; font-size:0.85rem;" onclick="setPresetVoltage(3.3)">3.3V</button>
                    <button class="toggle-btn" style="padding:6px 10px; font-size:0.85rem;" onclick="setPresetVoltage(5)">5V</button>
                    <button class="toggle-btn" style="padding:6px 10px; font-size:0.85rem;" onclick="setPresetVoltage(12)">12V</button>
                    <button class="toggle-btn" style="padding:6px 10px; font-size:0.85rem;" onclick="setPresetVoltage(13.5)">13.5V</button>
                </div>
                <!-- Quick Set Buttons End -->
            </div>
            <button class="toggle-btn off" id="btn1" onclick="toggle(1)">Turn On</button>
        </div>
        <!-- Output 2: Fixed 5V -->
        <div class="output-card">
            <div class="output-header">
                <span class="output-title">Output 2 (5V Fixed)</span>
                <div class="status-indicator" id="status2"></div>
            </div>
            <div class="display" id="display2">5.0V</div>
            <button class="toggle-btn off" id="btn2" onclick="toggle(2)">Turn On</button>
        </div>
        <!-- Output 3: Fixed 3.3V -->
        <div class="output-card">
            <div class="output-header">
                <span class="output-title">Output 3 (3.3V Fixed)</span>
                <div class="status-indicator" id="status3"></div>
            </div>
            <div class="display" id="display3">3.3V</div>
            <button class="toggle-btn off" id="btn3" onclick="toggle(3)">Turn On</button>
        </div>
    </div>
    <button class="about-btn" onclick="showAbout()">ℹ️ About</button>
    <script>
        const state = {
            output1: false,
            output2: false,
            output3: false,
            voltage1: 2.5,
            warningDisabled: false,
            pendingToggle: null
        };
        const slider = document.getElementById('voltage1');
        const display = document.getElementById('display1');
        slider.addEventListener('input', function() {
            state.voltage1 = parseFloat(this.value);
            display.textContent = state.voltage1.toFixed(1) + 'V';
            sendCommand('set_voltage', 1, state.voltage1);
        });
        // Quick Set voltage buttons
        function setPresetVoltage(value) {
            slider.value = value;
            state.voltage1 = value;
            display.textContent = value.toFixed(1) + 'V';
            sendCommand('set_voltage', 1, value);
        }
        display.textContent = '2.5V';
        function toggle(output) {
            const stateKey = 'output' + output;
            if (output === 1 && !state[stateKey] && state.voltage1 > 5 && !state.warningDisabled) {
                state.pendingToggle = output;
                document.getElementById('warningModal').classList.add('show');
                return;
            }
            executeToggle(output);
        }
        function executeToggle(output) {
            const btn = document.getElementById('btn' + output);
            const status = document.getElementById('status' + output);
            const stateKey = 'output' + output;
            state[stateKey] = !state[stateKey];
            if (state[stateKey]) {
                btn.classList.remove('off'); btn.classList.add('on');
                btn.textContent = 'Turn Off'; status.classList.add('on');
            } else {
                btn.classList.remove('on'); btn.classList.add('off');
                btn.textContent = 'Turn On'; status.classList.remove('on');
            }
            sendCommand('toggle', output, state[stateKey] ? 1 : 0);
        }
        function cancelWarning() {
            document.getElementById('warningModal').classList.remove('show');
            state.pendingToggle = null;
        }
        function proceedWithToggle() {
            if (document.getElementById('dontShowAgain').checked) {
                state.warningDisabled = true;
            }
            document.getElementById('warningModal').classList.remove('show');
            if (state.pendingToggle) {
                executeToggle(state.pendingToggle);
                state.pendingToggle = null;
            }
        }
        function sendCommand(action, output, value) {
            fetch('/control', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'action=' + action + '&output=' + output + '&value=' + value
            }).catch(function() { console.log('Command sent'); });
        }
        function showAbout() {
            document.getElementById('aboutModal').classList.add('show');
        }
        function closeAbout() {
            document.getElementById('aboutModal').classList.remove('show');
        }
    </script>
</body>
</html>
)rawliteral";