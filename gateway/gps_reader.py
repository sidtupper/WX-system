import time
import serial
import pynmea2
import csv
import os

# Define serial port and baud rate
SERIAL_PORT = "/dev/serial0"
BAUD_RATE = 9600
CSV_FILE = "gps_log.csv"

# Path to the Green Activity LED on Raspberry Pi 4B
LED_PATH = "/sys/class/leds/led0/brightness"

def set_led(state):
    """Turns the green ACT LED on (1) or off (0)"""
    try:
        with open(LED_PATH, "w") as led:
            led.write(str(state))
    except IOError:
        # Fails safely if not run with sudo or path differs on older OS versions
        pass

def setup_led_override():
    """Tells the OS to let us control the LED manually"""
    try:
        with open("/sys/class/leds/led0/trigger", "w") as trigger:
            trigger.write("none")
    except IOError:
        pass

def restore_led_default():
    """Restores the LED to its default SD card activity behavior"""
    try:
        with open("/sys/class/leds/led0/trigger", "w") as trigger:
            trigger.write("mmc0")
    except IOError:
        pass

def read_gps():
    print("Initializing GPS logging... (Waiting for satellite fix)")
    setup_led_override()
    set_led(0) # Start with LED off

    # Check if CSV exists to decide if we need to write headers
    file_exists = os.path.isfile(CSV_FILE)

    try:
        ser = serial.Serial(SERIAL_PORT, baudrate=BAUD_RATE, timeout=0.5)
    except Exception as e:
        print(f"Error opening serial port: {e}")
        restore_led_default()
        return

    # Open CSV file in append mode
    with open(CSV_FILE, mode='a', newline='') as f:
        writer = csv.writer(f)
        
        # Write header row if it's a new file
        if not file_exists:
            writer.writerow(["Timestamp", "Latitude", "Lat_Dir", "Longitude", "Lon_Dir", "Speed_Knots"])

        while True:
            try:
                line = ser.readline().decode('utf-8', errors='replace').strip()
                
                if line.startswith('$GPRMC') or line.startswith('$GNGRM'):
                    try:
                        msg = pynmea2.parse(line)
                        
                        if msg.status == 'A':  # 'A' = Active Fix, 'V' = Void
                            # Flash the Green Pi LED quickly to signal data acquisition
                            set_led(1)
                            
                            # Clean up coordinates & log data
                            row = [
                                msg.timestamp,
                                f"{msg.latitude:.6f}",
                                msg.lat_dir,
                                f"{msg.longitude:.6f}",
                                msg.lon_dir,
                                msg.spd_over_grnd
                            ]
                            writer.writerow(row)
                            f.flush() # Force write data to the SD card immediately
                            
                            print(f"Logged Fix: {msg.timestamp} | Lat: {msg.latitude:.6f} | Lon: {msg.longitude:.6f}")
                            
                            time.sleep(0.1)
                            set_led(0) # Turn LED back off until next string arrives
                        else:
                            print("Waiting for satellite fix...", end="\r")
                            set_led(0)
                            
                    except pynmea2.ParseError:
                        continue
                        
                time.sleep(0.1)
                
            except KeyboardInterrupt:
                print("\nStopping GPS data logger.")
                ser.close()
                restore_led_default() # Reset LED back to system default
                break

if __name__ == "__main__":
    read_gps()

