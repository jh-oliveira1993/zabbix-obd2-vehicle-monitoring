import json
import uuid

def gen_uuid():
    return uuid.uuid4().hex

with open('/home/jose/Projects/python/obd2_zabbix/zabbix/generic_telemetry_by_obd2.json', 'r') as f:
    data = json.load(f)

dr = data['zabbix_export']['templates'][0]['discovery_rules'][0]

new_triggers = [
    {
        "uuid": gen_uuid(),
        "expression": "timeleft(/Generic Telemetry by OBD2/obd[{#OBD_KEY}],30m,,10)<2h and last(/Generic Telemetry by OBD2/obd[{#OBD_KEY}])>10",
        "name": "Predictive: Fuel tank reaching reserve (10%) in less than 2 hours",
        "priority": "INFO",
        "description": "Calculates the rate of fuel consumption over the last 30 minutes and predicts when it will reach 10%.",
        "status": "1",
        "tags": [{"tag": "scope", "value": "predictive"}]
    },
    {
        "uuid": gen_uuid(),
        "expression": "forecast(/Generic Telemetry by OBD2/obd[{#OBD_KEY}],10m,,10m)>115",
        "name": "Predictive: Engine Overheating (Temp > 115C) predicted in 10 mins",
        "priority": "HIGH",
        "description": "Analyzes temperature slope over the last 10 minutes to forecast if it will exceed 115C in the next 10 minutes.",
        "status": "1",
        "tags": [{"tag": "scope", "value": "predictive"}]
    },
    {
        "uuid": gen_uuid(),
        "expression": "timeleft(/Generic Telemetry by OBD2/obd[{#OBD_KEY}],15m,,11.0)<30m and last(/Generic Telemetry by OBD2/obd[{#OBD_KEY}])<12.5",
        "name": "Predictive: Battery drain. Voltage will hit 11.0V in less than 30 mins",
        "priority": "WARNING",
        "description": "Analyzes voltage drop over the last 15 minutes. Predicts alternator failure or severe battery drain.",
        "status": "1",
        "tags": [{"tag": "scope", "value": "predictive"}]
    },
    {
        "uuid": gen_uuid(),
        "expression": "forecast(/Generic Telemetry by OBD2/obd[{#OBD_KEY}],30d,,30d)>25 or forecast(/Generic Telemetry by OBD2/obd[{#OBD_KEY}],30d,,30d)<-25",
        "name": "Predictive: Long Term Fuel Trim will exceed +-25% in 30 days",
        "priority": "WARNING",
        "description": "Analyzes the trend of LTFT over a month. Predicts if dirty injectors or a vacuum leak will trigger a Check Engine Light soon.",
        "status": "1",
        "tags": [{"tag": "scope", "value": "predictive"}]
    },
    {
        "uuid": gen_uuid(),
        "expression": "timeleft(/Generic Telemetry by OBD2/obd[{#OBD_KEY}],7d,,10000)<30d",
        "name": "Predictive: 10,000 km maintenance milestone in less than 30 days",
        "priority": "INFO",
        "description": "Analyzes driving habits to predict when the car will hit 10,000 km since the last DTC clear (assuming oil change sync).",
        "status": "1",
        "tags": [{"tag": "scope", "value": "predictive"}]
    }
]

# Zabbix JSON export uses "1" for DISABLED status
dr['item_prototypes'][0]['trigger_prototypes'].extend(new_triggers)

new_overrides = [
    {
        "name": "Predictive: Enable Fuel Level",
        "step": str(len(dr['overrides'])+1),
        "filter": {"evaltype": "AND", "conditions": [{"macro": "{#OBD_KEY}", "value": "fuel_level", "formulaid": "A"}]},
        "operations": [{"operationobject": "TRIGGER_PROTOTYPE", "operator": "LIKE", "value": "Fuel tank reaching reserve", "status": "ENABLED"}]
    },
    {
        "name": "Predictive: Enable Coolant Temp Forecast",
        "step": str(len(dr['overrides'])+2),
        "filter": {"evaltype": "AND", "conditions": [{"macro": "{#OBD_KEY}", "value": "^coolant_temp$|^oil_temp$", "operator": "MATCHES_REGEX", "formulaid": "A"}]},
        "operations": [{"operationobject": "TRIGGER_PROTOTYPE", "operator": "LIKE", "value": "Engine Overheating", "status": "ENABLED"}]
    },
    {
        "name": "Predictive: Enable Battery Drain",
        "step": str(len(dr['overrides'])+3),
        "filter": {"evaltype": "AND", "conditions": [{"macro": "{#OBD_KEY}", "value": "control_module_voltage", "formulaid": "A"}]},
        "operations": [{"operationobject": "TRIGGER_PROTOTYPE", "operator": "LIKE", "value": "Battery drain", "status": "ENABLED"}]
    },
    {
        "name": "Predictive: Enable LTFT Warning",
        "step": str(len(dr['overrides'])+4),
        "filter": {"evaltype": "AND", "conditions": [{"macro": "{#OBD_KEY}", "value": "long_fuel_trim_1", "formulaid": "A"}]},
        "operations": [{"operationobject": "TRIGGER_PROTOTYPE", "operator": "LIKE", "value": "Long Term Fuel Trim", "status": "ENABLED"}]
    },
    {
        "name": "Predictive: Enable Maintenance",
        "step": str(len(dr['overrides'])+5),
        "filter": {"evaltype": "AND", "conditions": [{"macro": "{#OBD_KEY}", "value": "distance_since_dtc_clear", "formulaid": "A"}]},
        "operations": [{"operationobject": "TRIGGER_PROTOTYPE", "operator": "LIKE", "value": "maintenance milestone", "status": "ENABLED"}]
    }
]

dr['overrides'].extend(new_overrides)

with open('/home/jose/Projects/python/obd2_zabbix/zabbix/generic_telemetry_by_obd2.json', 'w') as f:
    json.dump(data, f, indent=4)
