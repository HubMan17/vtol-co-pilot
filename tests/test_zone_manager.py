import json
import pytest

from src.navigation.zone_manager import NoFlyZone, ZoneManager


SAMPLE_POINTS = [[59.93, 30.31], [59.94, 30.32], [59.93, 30.33]]


@pytest.fixture
def manager(tmp_path):
    return ZoneManager(path=tmp_path / "zones.json")


def test_create_zone(manager):
    zone = manager.add_zone(points=SAMPLE_POINTS)
    assert zone.id
    assert zone.points == SAMPLE_POINTS
    assert zone.name == ""
    assert zone.description == ""
    assert zone.altitude is None
    assert zone.color == "#1E293B"
    assert zone.created
    assert zone.modified


def test_create_zone_with_all_params(manager):
    zone = manager.add_zone(
        points=SAMPLE_POINTS,
        name="Test Zone",
        description="A restricted area",
        altitude=500.0,
    )
    assert zone.name == "Test Zone"
    assert zone.description == "A restricted area"
    assert zone.altitude == 500.0


def test_remove_zone(manager):
    zone = manager.add_zone(points=SAMPLE_POINTS)
    assert manager.remove_zone(zone.id) is True
    assert manager.get_zone(zone.id) is None
    assert len(manager.get_all_zones()) == 0


def test_remove_nonexistent(manager):
    assert manager.remove_zone("nonexistent-id") is False


def test_update_zone_params(manager):
    zone = manager.add_zone(points=SAMPLE_POINTS, name="Old")
    updated = manager.update_zone(zone.id, name="New", altitude=300.0)
    assert updated is not None
    assert updated.name == "New"
    assert updated.altitude == 300.0
    assert updated.modified >= zone.created


def test_update_zone_points(manager):
    zone = manager.add_zone(points=SAMPLE_POINTS)
    new_points = [[60.0, 31.0], [60.1, 31.1], [60.0, 31.2]]
    updated = manager.update_zone_points(zone.id, new_points)
    assert updated is not None
    assert updated.points == new_points


def test_get_all_zones(manager):
    manager.add_zone(points=SAMPLE_POINTS, name="A")
    manager.add_zone(points=SAMPLE_POINTS, name="B")
    zones = manager.get_all_zones()
    assert len(zones) == 2
    names = {z.name for z in zones}
    assert names == {"A", "B"}


def test_save_and_load(tmp_path):
    path = tmp_path / "zones.json"
    m1 = ZoneManager(path=path)
    m1.add_zone(points=SAMPLE_POINTS, name="Saved Zone", altitude=200.0)
    m1.add_zone(points=[[1, 2], [3, 4], [5, 6]], description="desc")

    m2 = ZoneManager(path=path)
    zones = m2.get_all_zones()
    assert len(zones) == 2
    saved = next(z for z in zones if z.name == "Saved Zone")
    assert saved.points == SAMPLE_POINTS
    assert saved.altitude == 200.0
    other = next(z for z in zones if z.description == "desc")
    assert other.points == [[1, 2], [3, 4], [5, 6]]


def test_zone_defaults():
    zone = NoFlyZone(id="test", points=[[0, 0], [1, 1], [0, 1]])
    assert zone.name == ""
    assert zone.description == ""
    assert zone.altitude is None
    assert zone.color == "#1E293B"
    assert zone.created != ""
    assert zone.modified != ""
