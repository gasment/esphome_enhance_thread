import esphome.codegen as cg
from esphome.components import text_sensor
from esphome.components.openthread.const import (
    CONF_EXT_PAN_ID,
    CONF_NETWORK_KEY,
    CONF_NETWORK_NAME,
    CONF_PAN_ID,
)
import esphome.config_validation as cv
from esphome.const import CONF_CHANNEL, CONF_IP_ADDRESS, ENTITY_CATEGORY_DIAGNOSTIC
from esphome.types import ConfigType

CONF_BEST_ROUTER_LINK_QUALITY = "best_router_link_quality"
CONF_ROLE = "role"
CONF_RLOC16 = "rloc16"
CONF_EUI64 = "eui64"
CONF_EXT_ADDR = "ext_addr"
CONF_MESH_LOCAL_ADDRESS = "mesh_local_address"
CONF_RLOC_ADDRESS = "rloc_address"
CONF_LINK_LOCAL_ADDRESS = "link_local_address"


DEPENDENCIES = ["openthread"]

openthread_info_ns = cg.esphome_ns.namespace("openthread_info")
IPAddressOpenThreadInfo = openthread_info_ns.class_(
    "IPAddressOpenThreadInfo", text_sensor.TextSensor, cg.PollingComponent
)
BestRouterLinkQualityOpenThreadInfo = openthread_info_ns.class_(
    "BestRouterLinkQualityOpenThreadInfo", text_sensor.TextSensor, cg.PollingComponent
)
RoleOpenThreadInfo = openthread_info_ns.class_(
    "RoleOpenThreadInfo", text_sensor.TextSensor, cg.PollingComponent
)
AddressOpenThreadInfo = openthread_info_ns.class_(
    "AddressOpenThreadInfo", text_sensor.TextSensor, cg.PollingComponent
)
AddressKind = openthread_info_ns.enum("AddressKind", is_class=True)
Rloc16OpenThreadInfo = openthread_info_ns.class_(
    "Rloc16OpenThreadInfo", text_sensor.TextSensor, cg.PollingComponent
)
ExtAddrOpenThreadInfo = openthread_info_ns.class_(
    "ExtAddrOpenThreadInfo", text_sensor.TextSensor, cg.PollingComponent
)
Eui64OpenThreadInfo = openthread_info_ns.class_(
    "Eui64OpenThreadInfo", text_sensor.TextSensor, cg.Component
)
ChannelOpenThreadInfo = openthread_info_ns.class_(
    "ChannelOpenThreadInfo", text_sensor.TextSensor, cg.PollingComponent
)
NetworkNameOpenThreadInfo = openthread_info_ns.class_(
    "NetworkNameOpenThreadInfo", text_sensor.TextSensor, cg.PollingComponent
)
NetworkKeyOpenThreadInfo = openthread_info_ns.class_(
    "NetworkKeyOpenThreadInfo", text_sensor.TextSensor, cg.PollingComponent
)
PanIdOpenThreadInfo = openthread_info_ns.class_(
    "PanIdOpenThreadInfo", text_sensor.TextSensor, cg.PollingComponent
)
ExtPanIdOpenThreadInfo = openthread_info_ns.class_(
    "ExtPanIdOpenThreadInfo", text_sensor.TextSensor, cg.PollingComponent
)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_IP_ADDRESS): text_sensor.text_sensor_schema(
            IPAddressOpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1s")),
        cv.Optional(CONF_MESH_LOCAL_ADDRESS): text_sensor.text_sensor_schema(
            AddressOpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1s")),
        cv.Optional(CONF_RLOC_ADDRESS): text_sensor.text_sensor_schema(
            AddressOpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1s")),
        cv.Optional(CONF_LINK_LOCAL_ADDRESS): text_sensor.text_sensor_schema(
            AddressOpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1s")),
        cv.Optional(CONF_BEST_ROUTER_LINK_QUALITY): text_sensor.text_sensor_schema(
            BestRouterLinkQualityOpenThreadInfo,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ).extend(cv.polling_component_schema("10s")),
        cv.Optional(CONF_ROLE): text_sensor.text_sensor_schema(
            RoleOpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1s")),
        cv.Optional(CONF_RLOC16): text_sensor.text_sensor_schema(
            Rloc16OpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1s")),
        cv.Optional(CONF_EXT_ADDR): text_sensor.text_sensor_schema(
            ExtAddrOpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1s")),
        cv.Optional(CONF_EUI64): text_sensor.text_sensor_schema(
            Eui64OpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1h")),
        cv.Optional(CONF_CHANNEL): text_sensor.text_sensor_schema(
            ChannelOpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1s")),
        cv.Optional(CONF_NETWORK_NAME): text_sensor.text_sensor_schema(
            NetworkNameOpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1s")),
        cv.Optional(CONF_NETWORK_KEY): text_sensor.text_sensor_schema(
            NetworkKeyOpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1s")),
        cv.Optional(CONF_PAN_ID): text_sensor.text_sensor_schema(  # noqa: F821
            PanIdOpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1s")),
        cv.Optional(CONF_EXT_PAN_ID): text_sensor.text_sensor_schema(
            ExtPanIdOpenThreadInfo, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        ).extend(cv.polling_component_schema("1s")),
    }
)


async def setup_conf(config: dict, key: str) -> None:
    if conf := config.get(key):
        var = await text_sensor.new_text_sensor(conf)
        await cg.register_component(var, conf)


async def to_code(config: ConfigType) -> None:
    await setup_conf(config, CONF_IP_ADDRESS)
    for key, kind in (
        (CONF_MESH_LOCAL_ADDRESS, AddressKind.MESH_LOCAL),
        (CONF_RLOC_ADDRESS, AddressKind.RLOC),
        (CONF_LINK_LOCAL_ADDRESS, AddressKind.LINK_LOCAL),
    ):
        if conf := config.get(key):
            var = await text_sensor.new_text_sensor(conf)
            await cg.register_component(var, conf)
            cg.add(var.set_kind(kind))
    await setup_conf(config, CONF_BEST_ROUTER_LINK_QUALITY)
    await setup_conf(config, CONF_ROLE)
    await setup_conf(config, CONF_RLOC16)
    await setup_conf(config, CONF_EXT_ADDR)
    await setup_conf(config, CONF_EUI64)
    await setup_conf(config, CONF_CHANNEL)
    await setup_conf(config, CONF_NETWORK_NAME)
    await setup_conf(config, CONF_NETWORK_KEY)
    await setup_conf(config, CONF_PAN_ID)
    await setup_conf(config, CONF_EXT_PAN_ID)
