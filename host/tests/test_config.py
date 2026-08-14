"""Configuration loading, and the TLS policy from ADR-0011."""

from __future__ import annotations

import pytest

from wigwagd.config import BrokerConfig, Config


def _broker(**kw) -> BrokerConfig:
    b = BrokerConfig(**kw)
    b.resolve()
    return b


def test_defaults_are_a_working_local_setup():
    cfg = Config()
    cfg.broker.resolve()
    assert cfg.broker.host == "localhost"
    assert cfg.broker.port == 1883
    assert cfg.broker.tls is False
    assert cfg.listen_host == "127.0.0.1"
    assert cfg.listen_port == 9410


@pytest.mark.parametrize("host", ["localhost", "127.0.0.1", "::1", ""])
def test_loopback_brokers_default_to_no_tls(host):
    assert _broker(host=host).tls is False


@pytest.mark.parametrize("host", ["mqtt.example.com", "192.168.1.50", "10.0.0.2", "8.8.8.8"])
def test_remote_brokers_default_to_tls_on(host):
    """Pointing at a remote broker must not silently ship data in the clear."""
    b = _broker(host=host)
    assert b.tls is True
    assert b.port == 8883, "port should follow the TLS default"


def test_unresolvable_host_is_treated_as_remote():
    """Fail toward security: an unrecognisable host gets TLS, not plaintext."""
    assert _broker(host="not a hostname at all").tls is True


def test_explicit_tls_setting_always_wins():
    assert _broker(host="mqtt.example.com", tls=False).tls is False
    assert _broker(host="localhost", tls=True).tls is True


def test_explicit_port_always_wins():
    assert _broker(host="mqtt.example.com", port=1884).port == 1884


def test_plaintext_to_a_remote_broker_warns():
    warnings = _broker(host="mqtt.example.com", tls=False, username="u").warnings()
    assert any("in the clear" in w for w in warnings)


def test_skip_verify_warns_that_it_does_not_protect():
    warnings = _broker(host="mqtt.example.com", tls=True, insecure_skip_verify=True, username="u").warnings()
    assert any("not authenticating" in w for w in warnings)


def test_remote_without_credentials_warns():
    assert any("no username" in w for w in _broker(host="mqtt.example.com").warnings())


def test_local_plaintext_does_not_warn():
    assert _broker(host="localhost").warnings() == []


def test_toml_file_is_loaded(tmp_path):
    path = tmp_path / "config.toml"
    path.write_text(
        """
        [broker]
        host = "mqtt.example.com"
        port = 8884
        username = "greg"

        [topics]
        prefix = "desk"

        [daemon]
        listen_port = 9999
        session_ttl = 60
        """,
        encoding="utf-8",
    )
    cfg = Config.load(path)
    assert cfg.broker.host == "mqtt.example.com"
    assert cfg.broker.port == 8884
    assert cfg.broker.username == "greg"
    assert cfg.topic_prefix == "desk"
    assert cfg.listen_port == 9999
    assert cfg.session_ttl == 60
    assert cfg.source == str(path)


def test_missing_config_file_is_not_an_error(tmp_path):
    cfg = Config.load(tmp_path / "nope.toml")
    assert cfg.source == "defaults"
    assert cfg.broker.host == "localhost"


def test_env_overrides_the_file(tmp_path, monkeypatch):
    path = tmp_path / "config.toml"
    path.write_text('[broker]\nhost = "from-file"\n', encoding="utf-8")
    monkeypatch.setenv("WIGWAG_BROKER_HOST", "from-env")
    monkeypatch.setenv("WIGWAG_MQTT_PASSWORD", "secret")
    cfg = Config.load(path)
    assert cfg.broker.host == "from-env"
    assert cfg.broker.password == "secret"


def test_topic_building():
    cfg = Config()
    cfg.topic_prefix = "wigwag"
    assert cfg.topic("state") == "wigwag/state"
    assert cfg.topic("button") == "wigwag/button"


def test_topic_prefix_rejects_mqtt_wildcards(tmp_path):
    path = tmp_path / "c.toml"
    path.write_text('[topics]\nprefix = "wig+wag"\n', encoding="utf-8")
    with pytest.raises(ValueError, match="wildcard"):
        Config.load(path)


def test_empty_topic_prefix_is_rejected(tmp_path):
    path = tmp_path / "c.toml"
    path.write_text('[topics]\nprefix = "   "\n', encoding="utf-8")
    with pytest.raises(ValueError, match="empty"):
        Config.load(path)


@pytest.mark.parametrize("port", [0, -1, 70000])
def test_out_of_range_listen_port_is_rejected(tmp_path, port):
    path = tmp_path / "c.toml"
    path.write_text(f"[daemon]\nlisten_port = {port}\n", encoding="utf-8")
    with pytest.raises(ValueError, match="listen_port"):
        Config.load(path)


def test_nonpositive_ttl_is_rejected(tmp_path):
    path = tmp_path / "c.toml"
    path.write_text("[daemon]\nsession_ttl = 0\n", encoding="utf-8")
    with pytest.raises(ValueError, match="session_ttl"):
        Config.load(path)
