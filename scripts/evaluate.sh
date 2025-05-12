#!/bin/bash

# This script was written with the assistance of ChatGPT.

# Retry parameters
MAX_RETRIES=5  # Changed MAX_RETRIES to 5
RETRY_DELAY=10  # Seconds between retries

# Run stop-the-world.py at the very beginning to clear the environment
echo "$(date '+%Y-%m-%d %H:%M:%S') - Running stop-the-world.py to clear the environment."
python3 scripts/stop-the-world.py

# Function to run the Python script and monitor the log file for inactivity
run_with_log_monitor() {
    script=$1
    retries=0

    while [ $retries -lt $MAX_RETRIES ]; do
        # Create a log file that includes the retry count
        log_file="$script.retry_$retries.log"

        # Start the Python script in the background and capture its PID
        echo "$(date '+%Y-%m-%d %H:%M:%S') - Starting script $script. Logging to $log_file."
        python3 "$script" > "$log_file" 2>&1 &
        pid=$!  # Capture the PID after starting the script

        echo "$(date '+%Y-%m-%d %H:%M:%S') - Script $script started with PID $pid."

        # Monitor the log file for changes (every 1 minute)
        monitor_log_file "$log_file" $pid &
        monitor_pid=$!

        # Wait for the process to finish
        wait $pid
        script_exit_code=$?

        # Kill the background monitoring process if the script has finished
        if ps -p $monitor_pid > /dev/null; then
            kill $monitor_pid
            wait $monitor_pid 2>/dev/null
        fi

        # Check if the script was terminated or exited with a failure code
        if [ $script_exit_code -eq 0 ]; then
            echo "$(date '+%Y-%m-%d %H:%M:%S') - Script $script finished successfully."
            return 0
        else
            echo "$(date '+%Y-%m-%d %H:%M:%S') - Script $script exited with code $script_exit_code. Retrying..."
            retries=$((retries + 1))
            sleep $RETRY_DELAY

            # If the script failed, run kick-the-tires.py without timeout
            echo "$(date '+%Y-%m-%d %H:%M:%S') - Running kick-the-tires.py due to failure."
            python3 scripts/kick-the-tires.py
        fi
    done

    echo "$(date '+%Y-%m-%d %H:%M:%S') - Script $script failed after $MAX_RETRIES attempts. Proceeding to next script."
    return 0  # Continue to the next script, do not stop
}

# Monitor the log file to check if it's been updated
monitor_log_file() {
    log_file=$1
    pid=$2

    while ps -p $pid > /dev/null; do
        # If the log file doesn't exist yet, wait for it to be created
        if [ ! -f "$log_file" ]; then
            echo "$(date '+%Y-%m-%d %H:%M:%S') - Waiting for log file $log_file to be created..."
            sleep 5
            continue
        fi

        # Get the current system time and the last modification time of the log file
        current_time=$(date +%s)  # Current system time in seconds
        last_mod_time=$(stat -c %Y "$log_file")  # Last modification time of the log file

        # Compare the current system time with the last modification time
        if [ $((current_time - last_mod_time)) -gt 900 ]; then  # 15 minutes tolerance
            echo "$(date '+%Y-%m-%d %H:%M:%S') - No log update in the last 15 minutes. Handling timeout for PID $pid."
            handle_timeout_pid $pid  # Call the timeout handling logic if no log update
            return 1
        fi

        # Wait for 1 minute before checking again
        sleep 60
    done
}

# Custom logic to handle the PID when timeout occurs
handle_timeout_pid() {
    pid=$1
    echo "$(date '+%Y-%m-%d %H:%M:%S') - Handling timeout for PID $pid"

    # Step 1: Run stop-the-world script to reset environment if needed
    echo "$(date '+%Y-%m-%d %H:%M:%S') - Running stop-the-world script to reset environment."
    python3 scripts/stop-the-world.py

    # Step 2: Send SIGTERM to the script to gracefully terminate it
    echo "$(date '+%Y-%m-%d %H:%M:%S') - Sending SIGTERM to process $pid"
    kill -15 $pid  # SIGTERM: Graceful termination

    # Wait a few seconds for the process to exit after SIGTERM
    sleep 5

    # Step 3: If the script is still running, send SIGKILL
    if ps -p $pid > /dev/null; then
        echo "$(date '+%Y-%m-%d %H:%M:%S') - Process $pid did not respond to SIGTERM, sending SIGKILL."
        kill -9 $pid  # SIGKILL: Forced termination
    else
        echo "$(date '+%Y-%m-%d %H:%M:%S') - Process $pid terminated successfully with SIGTERM."
    fi
}

# Main logic: Running the scripts with log monitoring and retries
main() {
    scripts=(
        "scripts/2-1.py"
        "scripts/2-2.py"
        "scripts/6-2.py"
        "scripts/6-3.py"
        "scripts/6-4.py"
        "scripts/6-5-skewness.py"
        "scripts/6-5-dataset.py"
        "scripts/6-5-cores.py"
        "scripts/6-5-slots.py"
    )

    # Iterate through each script
    for script in "${scripts[@]}"; do
        run_with_log_monitor "$script"  # Continue with the next script even if one fails
    done
}

# Execute the main logic
main
