![AI-Assisted Development](https://img.shields.io/badge/AI--Assisted-Development-informational)
# Heart Alarm

A Pebble alarm clock watchapp that heavy sleepers or chronic snoozers will both love and hate. If Rip Van Winkle had both a Pebble watch and this alarm app he would have woken up after the first night very confused and stressed out.

Do you struggle to wakeup on time? Hit snooze over and over? Is your sleepy brain really good at figuring out how to disarm all those clever little extreme alarm clock apps you've tried over the years? Well I have both a solution and a recurring early morning heart attack for you. 

Heart Alarm is an alarm clock app that will measure your heart rate against a set target BPM and require you maintain it for the entire configured sustain duration, otherwise it will let an ear splitting tone until you try harder. It has a range of customization and will support a large number of alarms though honestly you won't need to arm multiple of these in the morning.

## Images

### Configuration
<img width="200" height="228" alt="2026-08-16 23 45 41" src="https://github.com/user-attachments/assets/900bcae5-ab6e-4769-a1f2-abf91205753b" />
<img width="200" height="228" alt="2026-08-16 23 45 36" src="https://github.com/user-attachments/assets/3c1b3b66-60f4-4b1c-930b-2b3d0c68d563" />


### Alarm Sequence
<img width="200" height="228" alt="2026-08-16 23 45 26" src="https://github.com/user-attachments/assets/68525785-9405-4746-aef1-80ce69e2e0b8" />
<img width="200" height="228" alt="2026-08-16 23 45 13" src="https://github.com/user-attachments/assets/82c90057-64c5-4dfe-9038-82b062b54aa9" />
<img width="200" height="228" alt="2026-08-16 23 45 21" src="https://github.com/user-attachments/assets/8d648677-b2ae-4353-b50f-d42c836fdb5b" />
<img width="200" height="228" alt="2026-08-16 23 45 31" src="https://github.com/user-attachments/assets/2d8b98c4-f11d-459a-b5f7-35a5e3d4c328" />


## Features

- **Aggressive buzzing and punishing tone** - if you're heartbeat wasn't at the target BPM before it certainty will after hearing the alarm tone
- **Grace period** - configurable window giving you time to wakeup, scramble out of bed, get somewhere sound proof, and start revving your heartrate
- **3 supported alarm types**
  -  **once** - firing once then being disabled
  -  **temporary** - firing once then being deleted
  -  **repeat** - firing on a weekly schedule
- **Force-quit prevention** - like an unkillable zombie force-quitting the app will not work. After a moment the app rearms and starts again.
- **Emergency Timeout** - if you were too optimistic and now you can't hit the configured BPM target, worry not, after 5 minutes the alarm will disable itself to avoid ringing all day long

# Store
\<Coming soon\>

## Disclaimer

Transparently this app is predominately LLM written; I iterated, tested and triaged the underlying C but I can't claim to have written it.
I always prefer to either write the code myself or use lower level prompting coupled with back and forth questioning to teach myself and sharpen my skills.
But I do not have infinite time (wink) and needed an alarm that solved my needs. Without AI this project would not exist and I would still be snoozing 8 alarms every morning.
