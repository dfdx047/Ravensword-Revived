// Ravensword: Shadowlands - IL patches for the Mono assemblies (Unity 4.1.2).
// Usage: PatchAssemblies <originalManagedDir> <outputDir> <RavenMod.dll> <words.txt>
// The translation dictionary (words.txt, rebuilt from the user's APK) is written into a copy of
// RavenMod.dll in outputDir, in place of the placeholder it is compiled with.
// Values the player can change are read from RavenMod's ModCfg instead of being constants.
// Every patch asserts the IL shape it expects and aborts if it differs, so a new
// game version fails loudly instead of being silently half-patched.
using System;
using System.IO;
using System.Linq;
using Mono.Cecil;
using Mono.Cecil.Cil;

static class Program
{
    const int QualityIndex = 5;          // "Fantastic", the highest level
    const int ShaderLodHigh = 600;       // above every shader LOD in the game (max 400, water 501)
    const float OverlayCull = 200f;      // LAYER_WORLD_OVERLAY cull distance, game used 10

    static ModuleDefinition Mod;

    static FieldReference Cfg(ModuleDefinition m, string name) =>
        m.ImportReference(Field(Mod.Types.Single(t => t.Name == "ModCfg"), name));

    static MethodReference ModCall(ModuleDefinition m, string type, string name) =>
        m.ImportReference(Mod.Types.Single(t => t.Name == type).Methods.Single(x => x.Name == name));

    static void Main(string[] args)
    {
        if (args.Length != 4)
        {
            Console.Error.WriteLine("usage: PatchAssemblies <originalManagedDir> <outputDir> <RavenMod.dll> <words.txt>");
            Environment.Exit(2);
        }
        string inDir = args[0], outDir = args[1], modPath = args[2], wordsPath = args[3];
        Directory.CreateDirectory(outDir);

        var resolver = new DefaultAssemblyResolver();
        resolver.AddSearchDirectory(inDir);
        resolver.AddSearchDirectory(Path.GetDirectoryName(Path.GetFullPath(modPath)));
        var rp = new ReaderParameters { AssemblyResolver = resolver, ReadingMode = ReadingMode.Deferred };
        Mod = AssemblyDefinition.ReadAssembly(modPath, rp).MainModule;

        using (var asm = AssemblyDefinition.ReadAssembly(Path.Combine(inDir, "Assembly-CSharp-firstpass.dll"), rp))
        {
            var m = asm.MainModule;
            PatchLogoInit(m);
            PatchDeviceGen(m);
            PatchDetailPack(m);
            PatchMyGameMgr(m);
            PatchFarClip(m);
            PatchLayerCull(m);
            PatchFog(m);
            PatchControllers(m);
            PatchBilling(m);
            PatchGuiHooks(m);
            PatchLanguage(m);
            PatchPadBindings(m);
            PatchProjectedShadow(m);
            PatchPadPress(m);
            PatchFps(m, Method(m, "GameActorPlayer", "Awake"));
            asm.Write(Path.Combine(outDir, "Assembly-CSharp-firstpass.dll"));
        }
        Console.WriteLine("wrote Assembly-CSharp-firstpass.dll");

        using (var asm = AssemblyDefinition.ReadAssembly(Path.Combine(inDir, "Assembly-CSharp.dll"), rp))
        {
            PatchFps(asm.MainModule, Method(asm.MainModule, "OuyaGameObject", "Start"));
            asm.Write(Path.Combine(outDir, "Assembly-CSharp.dll"));
            Console.WriteLine("wrote Assembly-CSharp.dll");
        }

        WriteWords(modPath, Path.Combine(outDir, "RavenMod.dll"), File.ReadAllText(wordsPath), resolver);
    }

    const string WordsPlaceholder = "@@RAVENMOD_WORDS@@";

    static void WriteWords(string modPath, string outPath, string words, IAssemblyResolver resolver)
    {
        using (var asm = AssemblyDefinition.ReadAssembly(modPath, new ReaderParameters { AssemblyResolver = resolver, InMemory = true }))
        {
            int n = 0;
            foreach (var type in asm.MainModule.Types)
                foreach (var method in type.Methods.Where(x => x.HasBody))
                    foreach (var i in method.Body.Instructions.Where(x => x.OpCode == OpCodes.Ldstr && (string)x.Operand == WordsPlaceholder))
                    {
                        i.Operand = words;
                        n++;
                    }
            Expect(n == 1, "one use of the translation placeholder in RavenMod.dll");
            asm.Write(outPath);
        }
        Console.WriteLine("wrote RavenMod.dll with " + (words.Split('\u0001').Length / 2) + " translations");
    }

    // LogoInit.configureQualitySettings: the game caps Android at index 2 ("Simple") by RAM
    // and only enables projected character shadows when the GPU name contains "TEGRA".
    static void PatchLogoInit(ModuleDefinition m)
    {
        var method = Method(m, "LogoInit", "configureQualitySettings");
        var setLevel = Operand<MethodReference>(method, x => x.Name == "SetQualityLevel" && x.Parameters.Count == 2);
        var print = Operand<MethodReference>(method, x => x.Name == "print");
        var mgr = Type(m, "MyGameMgr");
        var shadowField = Field(mgr, "shadowMode");
        var advanced = shadowField.FieldType.Resolve().Fields.Single(f => f.Name == "ADVANCED");
        int advancedValue = Convert.ToInt32(advanced.Constant);

        var body = method.Body;
        body.Instructions.Clear();
        body.Variables.Clear();
        body.ExceptionHandlers.Clear();
        var il = body.GetILProcessor();
        il.Emit(OpCodes.Ldc_I4, advancedValue);
        il.Emit(OpCodes.Stsfld, shadowField);
        il.Emit(OpCodes.Ldc_I4, QualityIndex);
        il.Emit(OpCodes.Ldc_I4_1);
        il.Emit(OpCodes.Call, setLevel);
        il.Emit(OpCodes.Call, ModCall(m, "ModCfg", "Boot"));  // the player's settings go on top
        il.Emit(OpCodes.Ret);
        Log("LogoInit.configureQualitySettings -> quality " + QualityIndex + ", then ModCfg.Boot");
    }

    // GameMgr.detectDeviceGen: Android always ended up on GEN3. GEN6 is the highest non-PC tier.
    static void PatchDeviceGen(ModuleDefinition m)
    {
        var method = Method(m, "GameMgr", "detectDeviceGen");
        var type = Type(m, "GameMgr");
        var body = method.Body;
        body.Instructions.Clear();
        body.Variables.Clear();
        body.ExceptionHandlers.Clear();
        var il = body.GetILProcessor();
        il.Emit(OpCodes.Ldsfld, Field(type, "DEVICE_GEN6"));
        il.Emit(OpCodes.Stsfld, Field(type, "DEVICE_GEN"));
        il.Emit(OpCodes.Ret);
        Log("GameMgr.detectDeviceGen -> DEVICE_GEN6");
    }

    // DetailPack.Awake: vegetation/detail density and streaming radius.
    static void PatchDetailPack(ModuleDefinition m)
    {
        var method = Method(m, "DetailPack", "Awake");
        var half = One(method, i => i.OpCode == OpCodes.Ldc_R4 && (float)i.Operand == 0.5f, "detailLevel 0.5");
        half.OpCode = OpCodes.Ldsfld;
        half.Operand = Cfg(m, "DetailLevel");
        var il = method.Body.GetILProcessor();
        var first = method.Body.Instructions[0];
        il.InsertBefore(first, il.Create(OpCodes.Ldarg_0));
        il.InsertBefore(first, il.Create(OpCodes.Ldsfld, Cfg(m, "DetailRadius")));
        il.InsertBefore(first, il.Create(OpCodes.Stfld, Field(Type(m, "DetailPack"), "activeRadius")));
        Log("DetailPack.Awake -> detailLevel and activeRadius from settings");
    }

    // MyGameMgr: shader LOD cap and the view distance factor (the options slider overwrites
    // VIEW_FACTOR from a saved pref, so the assignment itself is patched, not just the default).
    static void PatchMyGameMgr(ModuleDefinition m)
    {
        var type = Type(m, "MyGameMgr");
        var cctor = type.Methods.Single(x => x.Name == ".cctor");
        var il = cctor.Body.GetILProcessor();

        var lodStore = One(cctor, i => i.OpCode == OpCodes.Stsfld && ((FieldReference)i.Operand).Name == "SHADER_LOD_HIGH", "SHADER_LOD_HIGH store");
        Expect(lodStore.Previous.OpCode == OpCodes.Ldc_I4 && (int)lodStore.Previous.Operand == 250, "SHADER_LOD_HIGH == 250");
        lodStore.Previous.Operand = ShaderLodHigh;

        var vfInit = One(cctor, i => i.OpCode == OpCodes.Stsfld && ((FieldReference)i.Operand).Name == "VIEW_FACTOR", "VIEW_FACTOR init");
        Expect(vfInit.Previous.OpCode == OpCodes.Ldc_R4, "VIEW_FACTOR initial constant");
        vfInit.Previous.OpCode = OpCodes.Ldsfld;
        vfInit.Previous.Operand = Cfg(m, "ViewFactor");

        int patched = 0;
        foreach (var method in type.Methods.Where(x => x.HasBody && x.Name != ".cctor"))
        {
            foreach (var store in method.Body.Instructions.Where(i => i.OpCode == OpCodes.Stsfld && ((FieldReference)i.Operand).Name == "VIEW_FACTOR").ToList())
            {
                var p = method.Body.GetILProcessor();
                p.InsertBefore(store, p.Create(OpCodes.Pop));
                p.InsertBefore(store, p.Create(OpCodes.Ldsfld, Cfg(m, "ViewFactor")));
                patched++;
            }
        }
        Expect(patched == 1, "exactly one VIEW_FACTOR assignment outside .cctor, found " + patched);
        Log("MyGameMgr -> SHADER_LOD_HIGH " + ShaderLodHigh + ", VIEW_FACTOR from settings");
    }

    // GameActorPlayer.updateViewFactor: absolute far clip after the game's own logic.
    static void PatchFarClip(ModuleDefinition m)
    {
        var method = Method(m, "GameActorPlayer", "updateViewFactor");
        var seventy = One(method, i => i.OpCode == OpCodes.Ldc_R4 && (float)i.Operand == 70f, "farClipPlane = 70");
        var setter = seventy.Next;
        Expect(setter.Operand is MethodReference r && r.Name == "set_farClipPlane", "set_farClipPlane after 70f");
        var load1 = seventy.Previous.Previous;
        var load2 = seventy.Previous;

        // Branches in the method target the final ret, so turn that ret into the first new
        // instruction instead of inserting before it; otherwise the new code would be skipped.
        var body = method.Body;
        var ret = body.Instructions.Last();
        Expect(ret.OpCode == OpCodes.Ret, "method ends in ret");
        var il = body.GetILProcessor();
        ret.OpCode = load1.OpCode;
        ret.Operand = load1.Operand;
        il.Append(il.Create(load2.OpCode, (dynamic)load2.Operand));
        il.Append(il.Create(OpCodes.Ldsfld, Cfg(m, "Far")));
        il.Append(il.Create(setter.OpCode, (MethodReference)setter.Operand));
        il.Append(il.Create(OpCodes.Ret));
        Log("GameActorPlayer.updateViewFactor -> farClipPlane from settings");
    }

    // LayerCullConfig: overlay layer cull distance. NPC distance is deliberately untouched:
    // maxNpcRange derives from it and activates/deactivates NPCs, which is gameplay.
    static void PatchLayerCull(ModuleDefinition m)
    {
        foreach (var name in new[] { "configureCulling", "setDefaultCulling" })
        {
            var ten = One(Method(m, "LayerCullConfig", name), i => i.OpCode == OpCodes.Ldc_R4 && (float)i.Operand == 10f, name + " overlay 10f");
            ten.Operand = OverlayCull;
        }
        Log("LayerCullConfig -> overlay cull " + OverlayCull);
    }

    // ConfigureFog.configureMaterial: without scaling fog, a longer far clip only shows
    // fully fogged silhouettes. Applies to the four _FogNear/_FogFar SetFloat calls.
    static void PatchFog(ModuleDefinition m)
    {
        var method = Method(m, "ConfigureFog", "configureMaterial");
        var calls = method.Body.Instructions.Where(i => i.Operand is MethodReference r && r.Name == "SetFloat").ToList();
        Expect(calls.Count == 4, "four SetFloat calls in configureMaterial, found " + calls.Count);
        var il = method.Body.GetILProcessor();
        foreach (var call in calls)
        {
            il.InsertBefore(call, il.Create(OpCodes.Ldsfld, Cfg(m, "FogScale")));
            il.InsertBefore(call, il.Create(OpCodes.Mul));
        }
        Log("ConfigureFog.configureMaterial -> fog scale from settings");
    }

    // The MOGA SDK starts a service with an implicit intent, which Android rejects with an
    // exception on modern targets, and no current gamepad uses it. Standard Android input
    // covers all of them. Any gamepad name missing from the game's 2013 list used to leave
    // controllerType None, and then initControllerInput dereferenced nulls; those now get the
    // Shield mapping, whose axes (right stick Z/RZ, triggers LTRIGGER/RTRIGGER) match current pads.
    static void PatchControllers(ModuleDefinition m)
    {
        var detect = Method(m, "InputHandler", "detectMogaController");
        detect.Body.Instructions.Clear();
        detect.Body.Variables.Clear();
        detect.Body.ExceptionHandlers.Clear();
        var il = detect.Body.GetILProcessor();
        il.Emit(OpCodes.Ldc_I4_0);
        il.Emit(OpCodes.Ret);

        foreach (var name in new[] { "Awake", "Update" })
        {
            var method = Method(m, "MogaInput", name);
            method.Body.Instructions.Clear();
            method.Body.Variables.Clear();
            method.Body.ExceptionHandlers.Clear();
            method.Body.GetILProcessor().Emit(OpCodes.Ret);
        }

        var handler = Type(m, "InputHandler");
        var typeField = Field(handler, "controllerType");
        var enumType = typeField.FieldType.Resolve();
        int none = Convert.ToInt32(enumType.Fields.Single(f => f.Name == "None").Constant);
        int generic = Convert.ToInt32(enumType.Fields.Single(f => f.Name == "AndroidGeneric").Constant);
        int shield = Convert.ToInt32(enumType.Fields.Single(f => f.Name == "ProjectShield").Constant);
        var dct = Method(m, "InputHandler", "detectControllerType");
        var init = Method(m, "ControllerInputProjectShield", "initializeInput");
        // Insert right before the closing Debug.Log("Detected controller type: ...").
        var ldstr = One(dct, i => i.OpCode == OpCodes.Ldstr && (string)i.Operand == "Detected controller type: ", "final log");
        var p = dct.Body.GetILProcessor();
        var apply = p.Create(OpCodes.Ldc_I4, shield);
        var isNone = new[] {
            p.Create(OpCodes.Ldsfld, typeField), p.Create(OpCodes.Ldc_I4, none), p.Create(OpCodes.Beq_S, apply),
            p.Create(OpCodes.Ldsfld, typeField), p.Create(OpCodes.Ldc_I4, generic), p.Create(OpCodes.Bne_Un_S, ldstr),
            apply, p.Create(OpCodes.Stsfld, typeField), p.Create(OpCodes.Call, init),
        };
        // Branches that targeted the log (the end of the if-chain) must run the new check too.
        foreach (var i in dct.Body.Instructions)
        {
            if (i.Operand == ldstr) i.Operand = isNone[0];
        }
        foreach (var i in isNone) p.InsertBefore(ldstr, i);

        // Stick dead zone, set in the panel.
        var range = Method(m, "ControllerInputInfo", "mapAxisRange");
        var rp = range.Body.GetILProcessor();
        var r0 = range.Body.Instructions[0];
        rp.InsertBefore(r0, rp.Create(OpCodes.Ldarg_1));
        rp.InsertBefore(r0, rp.Create(OpCodes.Call, ModCall(m, "ModInput", "Dz")));
        rp.InsertBefore(r0, rp.Create(OpCodes.Starg_S, range.Parameters[0]));

        // The touch scene holds both control schemes, so touch and gamepad can swap at runtime.
        var scene = One(Method(m, "MyGameMgr", "getStartGameSceneName"), i => i.OpCode == OpCodes.Ldstr && (string)i.Operand == "StartGame_controller", "controller scene");
        scene.Operand = "StartGame";
        Log("InputHandler -> MOGA disabled, modern gamepad mapping, dead zone, touch scene for all");
    }

    // Google IAB v3 (AIDL service, implicit intent) throws on current Android and no longer
    // exists on Play. Every entry point becomes a no-op: purchases are simply unavailable and
    // nothing is unlocked.
    static void PatchBilling(ModuleDefinition m)
    {
        int n = 0;
        foreach (var method in Type(m, "GoogleIAB").Methods.Where(x => x.IsPublic && x.IsStatic && x.HasBody))
        {
            Expect(method.ReturnType.FullName == "System.Void", "GoogleIAB." + method.Name + " returns void");
            method.Body.Instructions.Clear();
            method.Body.Variables.Clear();
            method.Body.ExceptionHandlers.Clear();
            method.Body.GetILProcessor().Emit(OpCodes.Ret);
            n++;
        }
        Log("GoogleIAB -> " + n + " methods disabled (billing unavailable)");
    }

    static void PatchFps(ModuleDefinition m, MethodDefinition method)
    {
        var setter = One(method, i => i.Operand is MethodReference r && r.Name == "set_targetFrameRate", "set_targetFrameRate");
        Expect(setter.Previous.OpCode == OpCodes.Ldc_I4_S || setter.Previous.OpCode == OpCodes.Ldc_I4, "constant before set_targetFrameRate");
        var fps = m.ImportReference(Field(Mod.Types.Single(t => t.Name == "ModCfg"), "Fps"));
        setter.Previous.OpCode = OpCodes.Ldsfld;
        setter.Previous.Operand = fps;
        Log(method.DeclaringType.Name + "." + method.Name + " -> targetFrameRate from settings");
    }

    // Connects the settings panel: an OnGUI on SimpleGUI (present in every scene), a per-frame
    // tick, knowing which menu is on screen, and no game input while the panel is open.
    // All game text reaches the screen through SimpleText.setText, after its init().
    static void PatchLanguage(ModuleDefinition m)
    {
        var set = Method(m, "SimpleText", "setText");
        var init = One(set, i => (i.OpCode == OpCodes.Call || i.OpCode == OpCodes.Callvirt) && ((MethodReference)i.Operand).Name == "init", "init call");
        var il = set.Body.GetILProcessor();
        var seq = new[] {
            il.Create(OpCodes.Ldarg_1),
            il.Create(OpCodes.Ldarg_0),
            il.Create(OpCodes.Ldfld, Field(Type(m, "SimpleText"), "textMesh")),
            il.Create(OpCodes.Call, ModCall(m, "ModLang", "Fit")),
            il.Create(OpCodes.Starg_S, set.Parameters[0]),
        };
        var after = init;
        foreach (var i in seq) { il.InsertAfter(after, i); after = i; }
        Log("SimpleText.setText -> translated through ModLang");
    }

    // The touch GUI scene has no gamepad bindings: each element gets those of the controller scene.
    static void PatchPadBindings(ModuleDefinition m)
    {
        var init = Method(m, "SimpleGUIElement", "init");
        var il = init.Body.GetILProcessor();
        var first = init.Body.Instructions[0];
        il.InsertBefore(first, il.Create(OpCodes.Ldarg_0));
        il.InsertBefore(first, il.Create(OpCodes.Call, ModCall(m, "ModInput", "Bind")));
        foreach (var name in new[] { "addElement", "addElementImmediate" })
        {
            var add = Method(m, "SimpleGUI", name);
            var p = add.Body.GetILProcessor();
            var f = add.Body.Instructions[0];
            p.InsertBefore(f, p.Create(OpCodes.Ldarg_1));
            p.InsertBefore(f, p.Create(OpCodes.Call, ModCall(m, "ModInput", "Bind")));
        }
        Log("SimpleGUIElement -> gamepad bindings of StartGame_controller");
    }

    // The hand icon: BtnActivate acts on BTN_RELEASED, the state a touch ends in, but a press from
    // a pad reached it as BTN_ACTIVATED and did nothing. The scene for pads used another button class.
    static void PatchPadPress(ModuleDefinition m)
    {
        var sim = Method(m, "SimplePushButton", "simulatePress");
        var set = sim.Body.Instructions.First(i => i.OpCode == OpCodes.Stfld && ((FieldReference)i.Operand).Name == "btnState");
        var state = set.Previous;
        Expect(state.OpCode.Code.ToString().StartsWith("Ldc_I4"), "constant before the first btnState store");
        var il = sim.Body.GetILProcessor();
        il.InsertBefore(state, il.Create(OpCodes.Ldarg_0));
        state.OpCode = OpCodes.Call;
        state.Operand = ModCall(m, "ModInput", "PressState");
        Log("SimplePushButton.simulatePress -> same button state as a touch");

        // Jumping: the instant button reaches the handlers as a press when touched, as a release from a pad.
        var instant = Method(m, "SimplePushInstantButton", "simulatePress");
        var first = instant.Body.Instructions.First(i => i.OpCode == OpCodes.Stfld && ((FieldReference)i.Operand).Name == "btnState");
        var released = first.Previous;
        Expect(released.OpCode.Code.ToString().StartsWith("Ldc_I4"), "constant before the btnState store");
        var releasedOp = released.OpCode;
        var releasedArg = released.Operand;
        var stateField = (FieldReference)first.Operand;
        released.OpCode = OpCodes.Call;
        released.Operand = ModCall(m, "ModInput", "InstantState");
        var end = instant.Body.Instructions.Last();
        Expect(end.OpCode == OpCodes.Ret, "method ends in ret");
        var ip = instant.Body.GetILProcessor();
        end.OpCode = OpCodes.Ldarg_0;
        end.Operand = null;
        ip.Append(releasedArg == null ? ip.Create(releasedOp) : ip.Create(releasedOp, (dynamic)releasedArg));
        ip.Append(ip.Create(OpCodes.Stfld, stateField));
        ip.Append(ip.Create(OpCodes.Ret));
        Log("SimplePushInstantButton.simulatePress -> press first, as a touch");
    }

    // Sharper, wider, and casters refreshed four times a second instead of once.
    static void PatchProjectedShadow(ModuleDefinition m)
    {
        var late = Method(m, "CharacterShadow", "LateUpdate");
        var il = late.Body.GetILProcessor();
        var reads = late.Body.Instructions.Where(i => i.OpCode == OpCodes.Ldfld && ((FieldReference)i.Operand).Name == "textureSize").ToList();
        Expect(reads.Count == 2, "two reads of textureSize");
        var size = Cfg(m, "ProjSize");
        foreach (var read in reads)
        {
            read.Previous.OpCode = OpCodes.Nop;   // the ldarg.0 it consumed
            read.Previous.Operand = null;
            read.OpCode = OpCodes.Ldsfld;
            read.Operand = size;
        }
        var orient = Method(m, "CharacterShadow", "OrientToEncloseTarget");
        var ret = orient.Body.Instructions.Last();
        Expect(ret.OpCode == OpCodes.Ret, "method ends in ret");
        var op = orient.Body.GetILProcessor();
        ret.OpCode = OpCodes.Ldarg_0;
        ret.Operand = null;
        op.Append(op.Create(OpCodes.Call, ModCall(m, "ModShadow", "Fit")));
        op.Append(op.Create(OpCodes.Ret));
        var start = Method(m, "ShadowMgr", "Start");
        foreach (var i in start.Body.Instructions.Where(x => x.OpCode == OpCodes.Ldc_R4 && (float)x.Operand == 1f))
            i.Operand = 0.25f;
        Log("CharacterShadow -> shadow size and range from settings, casters refreshed 4x per second");
    }

    static void PatchGuiHooks(ModuleDefinition m)
    {
        var gui = Type(m, "SimpleGUI");
        var onGui = new MethodDefinition("OnGUI", MethodAttributes.Private | MethodAttributes.HideBySig, m.TypeSystem.Void);
        var il = onGui.Body.GetILProcessor();
        il.Emit(OpCodes.Call, ModCall(m, "ModPanel", "OnGUI"));
        il.Emit(OpCodes.Ret);
        gui.Methods.Add(onGui);

        var late = Method(m, "SimpleGUI", "LateUpdate");
        var lp = late.Body.GetILProcessor();
        lp.InsertBefore(late.Body.Instructions[0], lp.Create(OpCodes.Call, ModCall(m, "ModPanel", "Tick")));

        int tags = 0;
        foreach (var method in gui.Methods.Where(x => x.Name.StartsWith("showOnlyElementsWithTag") && x.HasBody && x.Parameters.Count > 0 && x.Parameters[0].ParameterType.FullName == "System.String"))
        {
            var p = method.Body.GetILProcessor();
            var first = method.Body.Instructions[0];
            p.InsertBefore(first, p.Create(OpCodes.Ldarg_1));
            p.InsertBefore(first, p.Create(OpCodes.Call, ModCall(m, "ModPanel", "OnTag")));
            tags++;
        }
        Expect(tags >= 1, "showOnlyElementsWithTag overloads, found " + tags);

        // Touch and gamepad both stay alive: ModInput dispatches them and only follows the last
        // device used. The original code runs only for the desktop input modes.
        var update = Method(m, "InputHandler", "onUpdate");
        var up = update.Body.GetILProcessor();
        var start = update.Body.Instructions[0];
        var isOpen = m.ImportReference(Field(Mod.Types.Single(t => t.Name == "ModPanel"), "IsOpen"));
        var dispatch = ModCall(m, "ModInput", "OnUpdate");
        var cont = up.Create(OpCodes.Ldarg_0);
        up.InsertBefore(start, up.Create(OpCodes.Ldsfld, isOpen));
        up.InsertBefore(start, up.Create(OpCodes.Brfalse, cont));
        up.InsertBefore(start, up.Create(OpCodes.Ret));
        up.InsertBefore(start, cont);
        up.InsertBefore(start, up.Create(OpCodes.Call, dispatch));
        up.InsertBefore(start, up.Create(OpCodes.Brfalse, start));
        up.InsertBefore(start, up.Create(OpCodes.Ret));

        // Whether touch buttons show no longer depends on the input mode, only on ModInput.
        var layer = Method(m, "SimpleGUI", "setElementLayer");
        var old = layer.Body.Instructions.ToList();
        var guiLayer = Operand<FieldReference>(layer, f => f.Name == "guiLayer");
        var hidden = Operand<FieldReference>(layer, f => f.Name == "hiddenIfController");
        var baseLayer = Operand<FieldReference>(layer, f => f.Name == "guiBaseLayer");
        var layerHidden = Operand<FieldReference>(layer, f => f.Name == "LAYER_HIDDEN");
        var getGo = Operand<MethodReference>(layer, f => f.Name == "get_gameObject");
        var setLayer = Operand<MethodReference>(layer, f => f.Name == "set_layer");
        var hide = m.ImportReference(Field(Mod.Types.Single(t => t.Name == "ModInput"), "HideTouch"));
        var lp2 = layer.Body.GetILProcessor();
        layer.Body.Instructions.Clear();
        layer.Body.ExceptionHandlers.Clear();
        var go = lp2.Create(OpCodes.Ldarg_1);
        var show = lp2.Create(OpCodes.Ldarg_0);
        var set = lp2.Create(OpCodes.Callvirt, setLayer);
        lp2.Append(lp2.Create(OpCodes.Ldarg_1));
        lp2.Append(lp2.Create(OpCodes.Ldfld, guiLayer));
        lp2.Append(lp2.Create(OpCodes.Ldc_I4_M1));
        lp2.Append(lp2.Create(OpCodes.Bgt, go));
        lp2.Append(lp2.Create(OpCodes.Ret));
        lp2.Append(go);
        lp2.Append(lp2.Create(OpCodes.Callvirt, getGo));
        lp2.Append(lp2.Create(OpCodes.Ldarg_1));
        lp2.Append(lp2.Create(OpCodes.Ldfld, hidden));
        lp2.Append(lp2.Create(OpCodes.Brfalse, show));
        lp2.Append(lp2.Create(OpCodes.Ldsfld, hide));
        lp2.Append(lp2.Create(OpCodes.Brfalse, show));
        lp2.Append(lp2.Create(OpCodes.Ldsfld, layerHidden));
        lp2.Append(lp2.Create(OpCodes.Br, set));
        lp2.Append(show);
        lp2.Append(lp2.Create(OpCodes.Ldfld, baseLayer));
        lp2.Append(lp2.Create(OpCodes.Ldarg_1));
        lp2.Append(lp2.Create(OpCodes.Ldfld, guiLayer));
        lp2.Append(lp2.Create(OpCodes.Add));
        lp2.Append(set);
        lp2.Append(lp2.Create(OpCodes.Ret));

        // A camera with this script tells the bridge when the world has been drawn. The class
        // is empty in the game, so it does nothing else where it is already used.
        var eyes = Type(m, "ModelEyes");
        var post = new MethodDefinition("OnPostRender", MethodAttributes.Private | MethodAttributes.HideBySig, m.TypeSystem.Void);
        var pp = post.Body.GetILProcessor();
        pp.Emit(OpCodes.Ldarg_0);
        pp.Emit(OpCodes.Call, ModCall(m, "ModFx", "OnPost"));
        pp.Emit(OpCodes.Ret);
        eyes.Methods.Add(post);
        Log("SimpleGUI/InputHandler -> settings panel connected (" + tags + " menu hooks)");
    }

    static TypeDefinition Type(ModuleDefinition m, string name) =>
        m.Types.SingleOrDefault(t => t.Name == name) ?? throw new Exception("type not found: " + name);

    static MethodDefinition Method(ModuleDefinition m, string type, string name) =>
        Type(m, type).Methods.SingleOrDefault(x => x.Name == name) ?? throw new Exception("method not found: " + type + "." + name);

    static FieldDefinition Field(TypeDefinition t, string name) =>
        t.Fields.SingleOrDefault(f => f.Name == name) ?? throw new Exception("field not found: " + t.Name + "." + name);

    static T Operand<T>(MethodDefinition method, Func<T, bool> pred) where T : class =>
        method.Body.Instructions.Select(i => i.Operand as T).First(x => x != null && pred(x));

    static Instruction One(MethodDefinition method, Func<Instruction, bool> pred, string what)
    {
        var hits = method.Body.Instructions.Where(pred).ToList();
        Expect(hits.Count == 1, method.Name + ": expected one match for " + what + ", found " + hits.Count);
        return hits[0];
    }

    static void Expect(bool cond, string what)
    {
        if (!cond) throw new Exception("IL pattern mismatch: " + what);
    }

    static void Log(string s) => Console.WriteLine("patched " + s);
}
