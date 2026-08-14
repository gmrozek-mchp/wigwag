"""wigwag host daemon.

Receives session-state reports from Claude Code hooks over loopback UDP, aggregates
them across sessions by urgency, and publishes the result as a retained MQTT message
for the device to consume.

See `docs/adr/` for the decisions behind each of those choices.
"""

__version__ = "0.1.0"
