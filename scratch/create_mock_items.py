import sys
import json
import requests

ZABBIX_URL = "https://jholiv-zabbix.ddns.net/zabbix/api_jsonrpc.php" 
USERNAME = "Antigravity"
PASSWORD = "FratzoX93!@Dyrtor135"
HOST_NAME = "Volkswagen UP ESP32"

def rpc_call(method, params, auth=None):
    payload = {
        "jsonrpc": "2.0",
        "method": method,
        "params": params,
        "id": 1
    }
    if auth:
        payload["auth"] = auth
    
    # print(f"Payload: {json.dumps(payload, indent=2)}")
    response = requests.post(ZABBIX_URL, json=payload, headers={'Content-Type': 'application/json-rpc'})
    response.raise_for_status()
    res_json = response.json()
    if 'error' in res_json:
        print(f"Erro na API Zabbix: {res_json['error']['data']}")
        sys.exit(1)
    return res_json['result']

def main():
    print("Logando na API do Zabbix...")
    try:
        auth_token = rpc_call("user.login", {"username": USERNAME, "password": PASSWORD})
    except Exception as e:
        print(f"Falha de autenticação. URL está correta? Erro: {e}")
        return

    print("Buscando host:", HOST_NAME)
    hosts = rpc_call("host.get", {"filter": {"host": [HOST_NAME]}}, auth=auth_token)
    if not hosts:
        print(f"Erro: Host '{HOST_NAME}' não encontrado no Zabbix.")
        return
    
    host_id = hosts[0]['hostid']
    print(f"Host encontrado! ID: {host_id}")
    
    items_to_create = [
        {
            "name": "Mock Coolant Temp",
            "key_": "obd.numeric[coolant_temp]",
            "hostid": host_id,
            "type": 2, # Zabbix trapper
            "value_type": 0, # Numeric float
            "delay": "0"
        },
        {
            "name": "Mock RPM",
            "key_": "obd.numeric[rpm]",
            "hostid": host_id,
            "type": 2, # Zabbix trapper
            "value_type": 0, # Numeric float
            "delay": "0"
        }
    ]
    
    print("Criando itens Mock...")
    for item in items_to_create:
        try:
            rpc_call("item.create", item, auth=auth_token)
            print(f"  -> Item '{item['name']}' ({item['key_']}) criado com sucesso!")
        except SystemExit:
            print(f"  -> Falha ao criar '{item['name']}' (talvez já exista?)")

    # Logout
    rpc_call("user.logout", [], auth=auth_token)
    print("Itens criados. Pode conferir no painel (Latest Data)!")

if __name__ == "__main__":
    main()
