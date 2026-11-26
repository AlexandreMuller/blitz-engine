# Cheat-sheet de Migração BGE/UPBGE → O3DE (Lua)

Guia prático de conversão dos padrões de script mais comuns do Blender Game Engine /
UPBGE (Python) para O3DE (Lua). Cada seção mostra o **padrão BGE** e o **equivalente O3DE**
pronto para colar num projeto mínimo.

> **Aviso de versão**: APIs verificadas contra a documentação oficial do O3DE
> (docs.o3de.org, branch estável). Onde uma assinatura varia entre versões, está
> marcado com ⚠️ *verifique*.

---

## 0. Setup do projeto mínimo

1. Criar projeto a partir do template padrão (Project Manager ou
   `scripts/o3de.bat create-project --project-path ...`).
2. Gens necessárias (já vêm no template padrão na maioria dos casos):
   - **PhysX** — física, colisão, raycast
   - **StartingPointInput** — sistema de input (`.inputbindings`)
3. Estrutura sugerida:
   ```
   <projeto>/Assets/Scripts/     # scripts .lua
   <projeto>/Assets/Prefabs/     # prefabs spawnáveis
   <projeto>/Assets/Input/       # .inputbindings
   ```
4. Rodar o jogo sem abrir o editor (alinhado à sua arquitetura "Blender = editor"):
   ```
   <projeto>\build\windows\bin\profile\GameLauncher.exe +loadlevel Levels\Teste\Teste.spawnable
   ```

---

## 1. Estrutura básica de um script

### BGE (Python)

```python
import bge

def main(cont):
    own = cont.owner
    # roda a cada frame se o controller estiver ligado a um Always sensor
```

### O3DE (Lua)

```lua
-- Scripts/Mover.lua
local Mover = {
    Properties = {
        Speed = { default = 5.0, suffix = "m/s" }  -- aparece no inspector / exportador
    }
}

function Mover:OnActivate()
    -- setup, conectar EBuses
end

function Mover:OnTick(deltaTime, scriptTime)   -- só chamado se TickBus conectado
    -- lógica por frame
end

function Mover:OnDeactivate()
    -- desconectar tudo
end

return Mover
```

**Mudança de modelo mental**: no BGE o script roda quando o *sensor* dispara; no O3DE o
script é um **componente** com callbacks de ciclo de vida (`OnActivate`/`OnDeactivate`) e
eventos. Para "rodar todo frame", conecte o TickBus:

```lua
function Mover:OnActivate()
    self.tickHandler = TickBus.Connect(self)
end

function Mover:OnTick(deltaTime, scriptTime)
    -- deltaTime em segundos (equivale ao 'frame' do BGE, mas fracionário)
end

function Mover:OnDeactivate()
    self.tickHandler:Disconnect()
end
```

---

## 2. Movimento

### BGE

```python
def main(cont):
    own = cont.owner
    speed = 0.1
    own.applyMovement((speed, 0, 0), True)          # mover no eixo local X
    # ou:
    own.localPosition.x += speed
```

### O3DE — por frame com deltaTime

```lua
function Mover:OnActivate()
    self.tickHandler = TickBus.Connect(self)
end

function Mover:OnTick(deltaTime, scriptTime)
    local pos = TransformBus.Event.GetLocalPosition(self.entityId)
    pos.x = pos.x + self.Properties.Speed * deltaTime   -- velocidade real em m/s
    TransformBus.Event.SetLocalPosition(self.entityId, pos)
end

function Mover:OnDeactivate()
    self.tickHandler:Disconnect()
end
```

### O3DE — impulso em corpo rígido (equivalente a `applyImpulse`)

```lua
-- Requer PhysX Dynamic Rigid Body no mesmo entity
local impulse = Vector3(0, 0, 10)
RigidBodyRequestBus.Event.ApplyLinearImpulse(self.entityId, impulse)
```

### O3DE — rotação contínua (equivalente a `applyRotation`)

```lua
-- por frame:
TransformBus.Event.RotateAroundLocalZ(self.entityId, math.rad(90) * deltaTime)
```

---

## 3. Input de teclado/mouse

**Mudança cultural importante**: no BGE você faz *polling* a cada frame
(`keyboard.events[WKEY]`). No O3DE input é **event-driven** — você registra handlers e o
engine te chama. Não há loop de checagem.

### BGE

```python
import bge

keyboard = bge.logic.keyboard
just_activated = bge.logic.KX_INPUT_JUST_ACTIVATED

if keyboard.events[bge.events.WKEY] == just_activated:
    print("W pressionado")
if keyboard.events[bge.events.WKEY]:      # held
    print("W segurado")
```

### O3DE — passo 1: criar o `.inputbindings`

Crie um asset de input binding (Input Bindings Editor no O3DE Editor, ou gere via script)
mapeando teclas para **nomes de evento**:

| Event Name | Input (exemplo) |
|---|---|
| `move_forward` | tecla W |
| `jump` | tecla Espaço |
| `shoot` | botão esquerdo do mouse |

### O3DE — passo 2: conectar no Lua

```lua
local Player = { Properties = {} }

function Player:OnActivate()
    self.inputHandlers = {}

    self.inputHandlers.forward = {}
    self.inputHandlers.forward.OnPressed = function(_, value)
        Debug.Log("W pressionado")          -- equivale a JUST_ACTIVATED
    end
    self.inputHandlers.forward.OnHeld = function(_, value, deltaTime)
        -- mover enquanto segurado (chamado por frame)
    end
    self.inputHandlers.forward.OnReleased = function(_, value)
        Debug.Log("W solto")
    end

    self.buses = {}
    self.buses.forward = InputEventNotificationBus.Connect(
        self.inputHandlers.forward, InputEventNotificationId("move_forward"))
end

function Player:OnDeactivate()
    self.buses.forward:Disconnect()
end

return Player
```

**Variante** — conectar o `self` diretamente (quando o script responde a um único evento):

```lua
function Player:OnActivate()
    self.inputBus = InputEventNotificationBus.Connect(
        self, InputEventNotificationId("jump"))
end

function Player:OnPressed(value)      -- chamado pelo bus acima
    -- pular
end

function Player:OnDeactivate()
    self.inputBus:Disconnect()
end
```

> A entity precisa de um componente **Input** referenciando o arquivo `.inputbindings`.
> Múltiplos eventos no mesmo script: use `InputEventNotificationBus.GetCurrentBusId()`
> dentro do handler para saber qual evento disparou (ver docs "Synchronizing Animation
> Graphs" para o padrão completo).

---

## 4. Colisão

**Mudança de modelo**: o *Collision Sensor* do BGE vira um **evento do PhysX** entregue
ao script da própria entity. Requer componentes `PhysX Rigid Body` + `PhysX Collider`.

### BGE

```python
def main(cont):
    collision = cont.sensors["Collision"]
    if collision.positive:
        hit_object = collision.hitObject
        print("Colidiu com:", hit_object)
```

### O3DE — eventos de colisão no script

```lua
local Player = { Properties = {} }

-- Chamados automaticamente se esta entity tiver PhysX Rigid Body + Collider
function Player:OnCollisionBegin(collision)
    local other = collision.otherEntity     -- equivale a collision.hitObject ⚠️ verifique o
    Debug.Log("Colisão começou")             -- nome do campo na sua versão (Gems/PhysX)
end

function Player:OnCollisionPersist(collision)
    -- contato contínuo
end

function Player:OnCollisionEnd(collision)
    Debug.Log("Colisão terminou")
end

return Player
```

> ⚠️ **Verifique**: a convenção exata dos handlers de colisão em Lua
> (`OnCollisionBegin` etc. e o nome dos campos do `collisionEvent`) variou entre versões.
> Confira em `Gems/PhysX/Documentation` ou nos testes automatizados
> (`AutomatedTesting/Gem/PythonTests/physics/`) do seu checkout. Alternativa garantida:
> detectar por **raycast** (seção 6), que é 100% documentado.

### Triggers (equivalente a um sensor de colisão sem resposta física)

No O3DE, marque o collider como **Trigger** no componente PhysX Collider, e use:

```lua
function Player:OnTriggerEnter(triggerEvent)
    Debug.Log("Entrou na área")
end

function Player:OnTriggerExit(triggerEvent)
    Debug.Log("Saiu da área")
end
```

---

## 5. Spawn de objetos

**Mudança de modelo**: `addObject("Inimigo")` do BGE vira **spawn de prefab**.

### BGE

```python
import bge

scene = bge.logic.getCurrentScene()
obj = scene.addObject("Inimigo", own, 0)     # spawna no layer do own
obj.worldPosition = (0, 5, 1)
```

### O3DE — caminho documentado: Spawner Component

1. Crie um prefab (`Create Prefab...` no Entity Outliner ou via seu exportador Blender).
2. Crie uma entity "Spawner" com componente **Spawner**, apontando para o prefab.
3. No Lua:

```lua
function SpawnerScript:OnActivate()
    self.spawnBus = SpawnerComponentNotificationBus.Connect(self, self.entityId)
end

-- Dispara o spawn (ex.: num OnPressed de input)
function SpawnerScript:SpawnEnemy()
    SpawnerComponentRequestBus.Event.Spawn(self.entityId)
end

function SpawnerScript:OnEntitySpawned(ticket, entityId)
    -- a instância foi criada; entityId é a entity raiz do prefab
    TransformBus.Event.SetWorldTranslation(entityId, Vector3(0, 5, 1))
end

function SpawnerScript:OnDeactivate()
    self.spawnBus:Disconnect()
end
```

> ⚠️ **Verifique**: `SpawnerComponentRequestBus`/`SpawnerComponentNotificationBus` são a
> API herdada do Lumberyard; nas versões atuais o sistema canônico de spawn de prefabs é
> **Create Spawn Ticket / Spawn / Despawn** (usado via Script Canvas e exposto em C++).
> Se o bus acima não existir na sua versão, o caminho garantido é um pequeno graph
> ScriptCanvas com os nós **Create Spawn Ticket** → **Spawn** → (Delay) → **Despawn**
> (tutorial oficial "Spawn and Despawn a Prefab", docs.o3de.org).

---

## 6. Raycast (sensor de ray do BGE / mouse pick)

### BGE

```python
import bge

ray = cont.sensors["Ray"]
if ray.positive:
    hit_obj, hit_pos, hit_normal = ray.hitObject, ray.hitPosition, ray.hitNormal
```

### O3DE — raycast de cena (contra todos os colliders)

```lua
function Gun:Shoot()
    local request = RayCastRequest()
    request.Start = TransformBus.Event.GetWorldTranslation(self.entityId)
    request.Direction = Vector3(0, 1, 0)   -- normalizado; eixo "frente" do O3DE
    request.Distance = 100.0

    local hit = PhysXSystemRequestBus.Broadcast.RayCast(request)
    if hit then
        -- hit.EntityId, hit.Position, hit.Normal, hit.Distance ⚠️ verifique campos
        Debug.Log("Acertou algo a " .. tostring(hit.Distance) .. "m")
    end
end
```

### O3DE — raycast contra um corpo específico

```lua
local request = RayCastRequest()
request.Start = Vector3(5.0, 0.0, 5.0)
request.Direction = Vector3(0.0, 0.0, -1.0)
request.Distance = 10.0
local hit = SimulatedBodyComponentRequestBus.Event.RayCast(entityId, request)
```

---

## 7. Câmera

### BGE — câmera ativa e propriedades

```python
scene = bge.logic.getCurrentScene()
cam = scene.active_camera
cam.lens = 35                    # FOV/distância focal
scene.active_camera = outra_cam  # trocar câmera
```

### O3DE — configurar câmera por script

```lua
local CameraSetup = { Properties = {} }

function CameraSetup:OnActivate()
    CameraRequestBus.Event.SetFov(self.entityId, 85)              -- graus
    CameraRequestBus.Event.SetNearClipDistance(self.entityId, 0.2)
    CameraRequestBus.Event.SetFarClipDistance(self.entityId, 1024)
    -- Tornar esta câmera a ativa (equivale a scene.active_camera = cam):
    CameraRequestBus.Event.MakeActiveView(self.entityId)
end

return CameraSetup
```

A entity precisa de um componente **Camera**. Alternativamente, marque
**Make active camera on activation** no componente e nem precise de script para isso.

### O3DE — câmera seguindo o jogador (follow cam simples, estilo "câmera pai" do BGE)

```lua
local FollowCam = {
    Properties = {
        Target = { default = EntityId() },   -- arraste a entity do player
        Offset = { default = Vector3(0, -6, 3) },
        LerpSpeed = { default = 5.0 }
    }
}

function FollowCam:OnActivate()
    self.tickHandler = TickBus.Connect(self)
end

function FollowCam:OnTick(deltaTime, scriptTime)
    local targetPos = TransformBus.Event.GetWorldTranslation(self.Properties.Target)
    local desired = targetPos + self.Properties.Offset
    local current = TransformBus.Event.GetWorldTranslation(self.entityId)
    -- interpolação suave (Lerp)
    local t = math.min(self.Properties.LerpSpeed * deltaTime, 1.0)
    local newPos = current:Lerp(desired, t)
    TransformBus.Event.SetWorldTranslation(self.entityId, newPos)
    -- olhar para o alvo:
    TransformBus.Event.SetWorldRotationQuaternion(
        self.entityId, TransformBus.Event.GetWorldRotationQuaternion(self.Properties.Target))
end

function FollowCam:OnDeactivate()
    self.tickHandler:Disconnect()
end

return FollowCam
```

---

## 8. Diversos — equivalências rápidas

| BGE / `bge` | O3DE (Lua) |
|---|---|
| `own.localPosition` | `TransformBus.Event.GetLocalPosition(self.entityId)` / `SetLocalPosition` |
| `own.worldPosition` | `GetWorldTranslation` / `SetWorldTranslation` |
| `own.applyMovement((x,y,z), True)` | mover `GetLocalPosition` + delta por frame (seção 2) |
| `own.applyImpulse` | `RigidBodyRequestBus.Event.ApplyLinearImpulse` |
| `own.visible = False` | `SetVisibility` / desativar componente Mesh, ou `SetEntityActive` ⚠️ |
| `own.endObject()` | despawn do ticket (seção 5) ou `EntityUtilityBus`/`AZ::Interface` destroy |
| `bge.logic.addScene("Cena2")` | carregar outro level: `LoadLevel` (console/C++) ou troca de `.spawnable` |
| `bge.logic.getCurrentController()` | o próprio `self` do script do componente |
| `bge.logic.getCurrentScene().objects["X"]` | referência via `Properties` (`EntityId()`) ou busca por tag |
| `scene.post_draw` (HUD simples) | LyShine UI Canvas (`.uicanvas`) + `UiCanvasRefBus` |
| sensores de Delay | `self.tickHandler` + acumulador de tempo, ou Timing nodes |
| `mathutils.Vector` | `Vector3(x, y, z)` (já disponível no Lua do O3DE) |

---

## 9. Checklist do primeiro teste (30 min)

1. [ ] Projeto criado do template padrão, Gems PhysX + StartingPointInput ativas
2. [ ] 1 entity "Player": Transform + Mesh (cubo) + PhysX Dynamic Rigid Body + PhysX
      Collider + Lua Script (`Mover.lua`) + Input (com `.inputbindings`)
3. [ ] 1 entity "CameraRig": Camera + `FollowCam.lua` com Target = Player
4. [ ] 1 prefab "Inimigo" + 1 entity "Spawner" com Spawner component
5. [ ] `GameLauncher.exe +loadlevel ...` e validar: mover com W, câmera segue,
      spawn ao apertar tecla, colisão com chão
6. [ ] Comparar FPS/ comportamento com o mesmo protótipo no UPBGE

---

## Fontes (verificadas nesta data)

- docs.o3de.org — "Using Player Input in Open 3D Engine" (Lua input handlers)
- docs.o3de.org — "Input Component EBus Interface" (OnPressed/OnHeld/OnReleased)
- docs.o3de.org — "Camera Component" (CameraRequestBus Lua example)
- docs.o3de.org — "Simulated Bodies" (RayCastRequest Lua example)
- docs.o3de.org — "Scene Source Asset Best Practices" (FBX, escala, bake de animação)
- docs.o3de.org — "Spawn and Despawn a Prefab" (Create Spawn Ticket/Spawn/Despawn)
- o3de/AutomatedTesting — testes PhysX (`OnCollisionBegin` callbacks)
