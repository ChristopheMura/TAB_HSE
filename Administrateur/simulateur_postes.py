"""
Simulateur de Tab5 "postes" pour tester la Tab5 administrateur.
  pip install paho-mqtt
  python simulateur_postes.py --broker 172.20.10.2

Toutes les 3 s, pour les postes 1, 2, 3 :
  - publie  Detection         {"id":1,"Detection":{"Casque":..,"Gilet":..,"Chaussures":..}}
  - publie  Demande_critique  {"id":1,"Bouton":true}   (seulement si EPI manquant, au hasard)
Écoute     Autorisation_critique et affiche la décision de l'admin.
"""
import argparse, json, random, time
import paho.mqtt.client as mqtt

POSTES = [1, 2, 3]

def on_message(client, userdata, msg):
    data = json.loads(msg.payload)
    verdict = "AUTORISE" if data.get("Autorisation") else "REFUSE"
    print(f"<<< ADMIN -> poste {data.get('id')} : {verdict}   {msg.payload.decode()}")

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--broker", default="localhost")
    p.add_argument("--port", type=int, default=1883)
    a = p.parse_args()

    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, "simulateur-postes")
    c.on_message = on_message
    c.connect(a.broker, a.port)
    c.subscribe("Autorisation_critique")
    c.loop_start()

    while True:
        for pid in POSTES:
            det = {"Casque": random.random() > 0.25,
                   "Gilet": random.random() > 0.25,
                   "Chaussures": random.random() > 0.25}
            c.publish("Detection", json.dumps({"id": pid, "Detection": det}))
            print(f">>> Detection poste {pid} : {det}")
            if not all(det.values()) and random.random() > 0.6:
                c.publish("Demande_critique", json.dumps({"id": pid, "Bouton": True}))
                print(f">>> Demande critique poste {pid}")
        time.sleep(3)

if __name__ == "__main__":
    main()
