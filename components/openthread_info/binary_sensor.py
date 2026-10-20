"""Live Thread attachment diagnostics from the maintained OpenThread component."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

DEPENDENCIES = ["openthread"]
ns = cg.esphome_ns.namespace("openthread_info")
ThreadAttachOpenThreadInfo = ns.class_("ThreadAttachOpenThreadInfo", binary_sensor.BinarySensor, cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.Required("thread_attach"): binary_sensor.binary_sensor_schema(
        ThreadAttachOpenThreadInfo,
        device_class=DEVICE_CLASS_CONNECTIVITY,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ).extend(cv.COMPONENT_SCHEMA),
})

async def to_code(config):
    entry = config["thread_attach"]
    var = await binary_sensor.new_binary_sensor(entry)
    await cg.register_component(var, entry)
