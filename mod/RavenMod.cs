// In-game settings for the Ravensword mod: every graphics option lives here and is saved with
// PlayerPrefs. The patched game reads the public static fields of ModCfg instead of its constants.
// Hooks are added by scripts/patch_assemblies (SimpleGUI, InputHandler, LogoInit).
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using UnityEngine;

// The game ships a stripped mscorlib without these attributes, which the compiler always emits.
namespace System.Runtime.CompilerServices
{
    internal class CompilationRelaxationsAttribute : Attribute { public CompilationRelaxationsAttribute(int r) { } }
    internal class RuntimeCompatibilityAttribute : Attribute { public bool WrapNonExceptionThrows { get; set; } }
    internal enum MethodImplOptions { InternalCall = 4096 }
    internal class MethodImplAttribute : Attribute { public MethodImplAttribute(MethodImplOptions o) { } }
}
namespace System.Runtime.InteropServices
{
    internal class DllImportAttribute : Attribute { public DllImportAttribute(string name) { } }
}

// UnityEngine.dll was stripped too, but the engine still implements these. Mono binds internal
// calls by "Namespace.Class::method", so declaring them under the same names reaches them.
namespace UnityEngine
{
    internal static class QualitySettings
    {
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void set_vSyncCount(int v);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void set_antiAliasing(int v);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void set_anisotropicFiltering(int v);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void set_masterTextureLimit(int v);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void set_shadowDistance(float v);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void set_shadowCascades(int v);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void set_pixelLightCount(int v);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void set_lodBias(float v);
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void set_softVegetation(bool v);
    }

    internal static class Screen
    {
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int get_width();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern int get_height();
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void SetResolution(int width, int height, bool fullscreen, int refreshRate);
    }

    // An instance call in the engine: the object arrives as the first argument.
    internal static class GUIStyle
    {
        [MethodImpl(MethodImplOptions.InternalCall)] public static extern void set_fontSize(object style, int size);
    }
}

public class ModOpt
{
    public string Label, Key, Note;
    public float[] Vals;
    public string[] Text;
    public int Idx, Default;

    public float Val { get { return Vals[Idx]; } }
    public string Shown { get { return Text[Idx]; } }

    public void Load()
    {
        Idx = PlayerPrefs.GetInt(Key, Default);
        if (Idx < 0 || Idx >= Vals.Length) Idx = Default;
    }
}

public static class ModCfg
{
    // Read by patched game code.
    public static int Fps = 120;
    public static float Far = 2000f;
    public static float FogScale = 4f;
    public static float DetailRadius = 80f;
    public static float DetailLevel = 1f;
    public static float ViewFactor = 40f;
    public static float DeadZone = 0.15f;

    public static List<ModOpt> Opts;
    public static int ProjSize = 2048;
    public static float ProjRange = 60f;
    static ModOpt shadowTex, shadowRange, preset;
    static ModOpt language, fps, res, aa, aniso, tex, shadowMode, far, fog, vegRadius, vegDensity, fov, fullscreen, dead, touchUi;
    static ModOpt fxOn, bloom, ao, sunShadow, range, rays, tone, expo, contrast, sat, sharp, vig;
    static bool booted;

    const string Area = "aplica ao recarregar a área";
    const string Restart = "aplica ao reabrir o jogo";

    static ModOpt Add(string label, string key, float[] vals, string[] text, int def, string note)
    {
        var o = new ModOpt { Label = label, Key = "gm_" + key, Vals = vals, Text = text, Default = def, Note = note };
        o.Load();
        Opts.Add(o);
        return o;
    }

    // Eleven levels from "off" to 100%, so an effect can be set to taste.
    static ModOpt Level(string label, string key, float min, float max, int def)
    {
        var vals = new float[11];
        var text = new string[11];
        for (int i = 0; i <= 10; i++)
        {
            vals[i] = min + (max - min) * i / 10f;
            text[i] = i == 0 ? "Desligado" : (i * 10) + "%";
        }
        return Add(label, key, vals, text, def, null);
    }

    // The same, for a setting whose middle is the original look: 50% to 150%.
    static ModOpt Percent(string label, string key, float min, float max, int def)
    {
        var vals = new float[11];
        var text = new string[11];
        for (int i = 0; i <= 10; i++)
        {
            vals[i] = min + (max - min) * i / 10f;
            text[i] = (50 + i * 10) + "%";
        }
        return Add(label, key, vals, text, def, null);
    }

    static void Build()
    {
        Opts = new List<ModOpt>();
        preset = Add("Qualidade", "preset", new float[] { 0, 1, 2, 3, 4, 5 }, new[] { "Baixo", "Médio", "Alto", "Muito Alto", "Ultra", "Personalizado" }, 3, null);
        language = Add("Idioma", "lang", new float[] { 0, 1 }, new[] { "Português", "English" }, 0, "ao mudar, reinicie o jogo para aplicar em todos os textos");
        string[] onOff = { "Desligado", "Ligado" };
        float[] b = { 0f, 1f };
        fps = Add("Limite de FPS", "fps", new float[] { 30, 60, 90, 120 }, new[] { "30", "60", "90", "120" }, 1, null);
        fxOn = Add("Modo remaster", "fx", b, onOff, 1, null);
        res = Add("Resolução", "res5", new float[] { 0, 360, 480, 540, 720, 900, 1080, 1440, 1620, 2160 }, new[] { "Da tela", "360p", "480p", "540p", "720p", "900p", "1080p", "1440p (2K)", "1620p", "2160p (4K)" }, 0, "abaixo da tela fica mais leve, acima fica mais nítido e pesa mais; precisa do modo remaster ligado");
        bloom = Level("Bloom / brilho", "bloom3", 0f, 0.8f, 4);
        ao = Level("Sombra de contato (AO)", "ao4", 0f, 1f, 7);
        sunShadow = Level("Sombras do sol", "sun3", 0f, 1f, 10);
        range = Add("Alcance das sombras", "range3", new float[] { 30, 60, 100, 150, 200, 300, 500 }, new[] { "30 m", "60 m", "100 m", "150 m", "200 m", "300 m", "500 m" }, 3, null);
        rays = Level("Raios de sol", "rays3", 0f, 0.7f, 3);
        tone = Level("Cor de cinema (HDR)", "tone3", 0f, 1f, 10);
        expo = Percent("Exposição", "expo3", 0.5f, 1.5f, 5);
        contrast = Level("Contraste", "cont3", 0f, 0.5f, 3);
        sat = Level("Saturação", "sat3", 1f, 1.5f, 2);
        sharp = Level("Nitidez", "sharp3", 0f, 1f, 3);
        vig = Level("Vinheta (bordas escuras)", "vig3", 0f, 0.4f, 0);
        fullscreen = Add("Tela inteira / widescreen", "full", b, onOff, 1, null);
        fov = Add("Campo de visão (FOV)", "fov", new float[] { 50, 60, 70, 80, 90 }, new[] { "50°", "60° (original)", "70°", "80°", "90°" }, 1, null);
        aa = Add("Anti-aliasing", "aa2", new float[] { 0, 2, 4, 8 }, new[] { "Desligado", "2x", "4x", "8x" }, 2, null);
        aniso = Add("Filtro anisotrópico", "aniso", new float[] { 0, 1, 2 }, new[] { "Desligado", "Por textura", "Forçado" }, 2, null);
        tex = Add("Texturas", "tex", new float[] { 0, 1, 2 }, new[] { "Completas", "Metade", "Um quarto" }, 0, null);
        far = Add("Distância de render", "far2", new float[] { 200, 450, 700, 1000, 1500, 2000 }, new[] { "200 m", "450 m (original)", "700 m", "1000 m", "1500 m", "2000 m" }, 3, "acima de 1000 m superfícies distantes podem tremer");
        fog = Add("Distância do nevoeiro", "fog", new float[] { 1, 2, 4, 8 }, new[] { "x1 (original)", "x2", "x4", "x8" }, 2, null);
        vegRadius = Add("Raio da vegetação", "vegr", new float[] { 20, 40, 80, 120 }, new[] { "20 m (original)", "40 m", "80 m", "120 m" }, 2, null);
        vegDensity = Add("Densidade da vegetação", "vegd", new float[] { 0.2f, 0.5f, 1f }, new[] { "20% (original)", "50%", "100%" }, 2, null);
        shadowMode = Add("Sombras das personagens", "shmode", new float[] { 0, 1, 2 }, new[] { "Nenhuma", "Simples", "Projetadas" }, 2, Area);
        shadowTex = Add("Resolução da sombra projetada", "shtex", new float[] { 512, 1024, 2048, 4096 }, new[] { "512", "1024", "2048", "4096" }, 2, null);
        shadowRange = Add("Alcance da sombra projetada", "shrange", new float[] { 20, 30, 45, 60, 90, 120 }, new[] { "20 m", "30 m (original)", "45 m", "60 m", "90 m", "120 m" }, 3, null);
        touchUi = Add("Botões de touch", "touchui", new float[] { 0, 1, 2 }, new[] { "Troca automática", "Sempre visíveis", "Sempre escondidos" }, 0, "touch e controle funcionam sempre");
        dead = Add("Zona morta dos analógicos", "dead", new float[] { 0.05f, 0.1f, 0.15f, 0.25f }, new[] { "5%", "10%", "15%", "25%" }, 2, null);
    }

    // Called once per process, from LogoInit.configureQualitySettings.
    public static void Boot()
    {
        if (booted) return;
        booted = true;
        Build();
        // First run with quality levels: start from one instead of the loose values.
        if (PlayerPrefs.GetInt("presetSet5", 0) == 0)
        {
            ApplyPreset(4);
            fps.Idx = 1;
            res.Idx = 0;
            PlayerPrefs.SetInt("presetSet5", 1);
            Save();
        }
        Apply();
        ModSys.Fullscreen = fullscreen.Val > 0.5f;
        MonoBehaviour.print("[ravenmod] settings loaded, fps=" + Fps + " far=" + Far);
    }

    public static void EnsureBooted() { if (!booted) Boot(); }

    public static ModOpt Find(string key)
    {
        foreach (var o in Opts) if (o.Key == "gm_" + key) return o;
        return null;
    }

    // Quality levels, from Baixo to Ultra: index into each option's list of values.
    // What a level changes is what costs: the look (colour, contrast, exposure) is the same in all.
    // What a level changes is what costs: the look (colour, contrast, exposure) is the same in all.
    static readonly string[] PresetKeys = { "fx", "aa2", "aniso", "tex", "bloom3", "ao4", "sun3", "range3", "rays3", "tone3", "expo3", "cont3", "sat3", "sharp3", "vig3", "far2", "fog", "vegr", "vegd", "shmode", "shtex", "shrange" };
    static readonly int[][] PresetIdx = {
        new[] { 1, 0, 1, 1, 0, 0, 0, 0, 0, 10, 5, 2, 2, 0, 0, 1, 0, 0, 0, 2, 0, 0 },
        new[] { 1, 1, 2, 0, 1, 3, 0, 1, 0, 10, 5, 2, 2, 2, 0, 2, 1, 1, 1, 2, 0, 1 },
        new[] { 1, 2, 2, 0, 2, 5, 5, 2, 2, 10, 5, 2, 2, 3, 0, 2, 2, 2, 1, 2, 1, 2 },
        new[] { 1, 2, 2, 0, 2, 7, 8, 3, 3, 10, 5, 2, 2, 3, 0, 3, 2, 2, 2, 2, 2, 3 },
        new[] { 1, 3, 2, 0, 2, 8, 10, 4, 3, 10, 5, 2, 2, 3, 0, 3, 2, 3, 2, 2, 2, 3 },
    };
    const int Custom = 5;

    public static bool IsGraphics(ModOpt o)
    {
        string key = o.Key.Substring(3);
        return key == "fov" || System.Array.IndexOf(PresetKeys, key) >= 0;
    }

    static void ApplyPreset(int level)
    {
        for (int i = 0; i < PresetKeys.Length; i++)
        {
            var o = Find(PresetKeys[i]);
            o.Idx = Mathf.Clamp(PresetIdx[level][i], 0, o.Vals.Length - 1);
        }
        preset.Idx = level;
    }

    // Any change by hand turns the level into Personalizado; going through levels skips that entry.
    public static void Change(ModOpt o, int dir)
    {
        if (o == preset)
        {
            ApplyPreset(o.Idx == Custom ? (dir > 0 ? 0 : Custom - 1) : (o.Idx + dir + Custom) % Custom);
            return;
        }
        o.Idx = (o.Idx + dir + o.Vals.Length) % o.Vals.Length;
        if (IsGraphics(o) && o.Key != "gm_fov") preset.Idx = Custom;
    }

    public static void Save()
    {
        foreach (var o in Opts) PlayerPrefs.SetInt(o.Key, o.Idx);
        PlayerPrefs.Save();
    }

    public static void Reset()
    {
        foreach (var o in Opts) o.Idx = o.Default;
    }

    static float worldFog = -1f, worldRadius = -1f, worldFactor = -1f, worldDensity = -1f;
    const BindingFlags AnyField = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;

    // What the game only reads while loading an area has to be handed over again on a change.
    static void ApplyWorld()
    {
        if (worldFog != FogScale)
        {
            worldFog = FogScale;
            foreach (Object o in Object.FindObjectsOfType(typeof(ConfigureFog))) ((ConfigureFog)o).onConfigure();
        }
        if (worldDensity != DetailLevel)
        {
            bool first = worldDensity < 0f;
            worldDensity = DetailLevel;
            if (!first)
            {
                foreach (Object o in Object.FindObjectsOfType(typeof(DetailPack)))
                {
                    var pack = (DetailPack)o;
                    foreach (DetailPackInfo info in pack.getObjectPool()) Object.Destroy(info.obj);
                    pack.detailLevel = DetailLevel;
                    pack.initQuadTree();
                    pack.onUpdate();
                }
            }
        }
        if (worldRadius != DetailRadius)
        {
            worldRadius = DetailRadius;
            var last = typeof(DetailPack).GetField("posLastQuery", AnyField);
            foreach (Object o in Object.FindObjectsOfType(typeof(DetailPack)))
            {
                var pack = (DetailPack)o;
                pack.activeRadius = DetailRadius;
                last.SetValue(pack, new Vector3(-10000f, -10000f, -10000f));
                pack.onUpdate();
            }
        }
        if (worldFactor != ViewFactor)
        {
            worldFactor = ViewFactor;
            MyGameMgr.VIEW_FACTOR = ViewFactor;
            Camera cam = Camera.main;
            if (cam != null) foreach (Object o in Object.FindObjectsOfType(typeof(LayerCullConfig))) ((LayerCullConfig)o).configureCulling(cam);
        }
    }

    public static void Apply()
    {
        Fps = (int)fps.Val;
        Far = far.Val;
        FogScale = fog.Val;
        DetailRadius = vegRadius.Val;
        DetailLevel = vegDensity.Val;
        DeadZone = dead.Val;
        ViewFactor = Far / 50f;  // world objects cull at distWorldObject * factor
        // Up to 60 the bridge paces frames itself; Unity's own limit would undo its catching up.
        ModFx.PacedFps = Fps <= 60 ? Fps : 0;
        Application.targetFrameRate = Fps <= 60 ? 120 : Fps;
        QualitySettings.set_vSyncCount(0);
        ModInput.TouchUi = (int)touchUi.Val;
        ModFx.Apply(fxOn.Val > 0.5f, (int)res.Val, (int)aa.Val, (int)aniso.Val, bloom.Val, ao.Val, sunShadow.Val, rays.Val, tone.Val, sat.Val, sharp.Val, vig.Val, expo.Val, contrast.Val, range.Val);
        QualitySettings.set_anisotropicFiltering((int)aniso.Val);
        QualitySettings.set_masterTextureLimit((int)tex.Val);
        ModLang.On = language.Val < 0.5f;
        ProjSize = (int)shadowTex.Val;
        ProjRange = shadowRange.Val;
        // These made no difference in any scene measured: the game has no LOD groups and its
        // shaders ignore pixel lights. They stay at their best instead of being options.
        QualitySettings.set_pixelLightCount(8);
        QualitySettings.set_lodBias(10f);
        QualitySettings.set_softVegetation(true);
        MyGameMgr.shadowMode = (MyGameMgr.ShadowMode)(int)shadowMode.Val;
        GameActorPlayer.DEFAULT_FOV = fov.Val;
        ModSys.Fullscreen = fullscreen.Val > 0.5f;
        if (GameActorPlayer.instance != null)
        {
            ApplyWorld();
            GameActorPlayer.instance.updateViewFactor();
            Camera cam = Camera.main;
            if (cam != null) cam.fieldOfView = fov.Val;
        }
    }
}

// Android window state. The system bars come back after focus changes, so it is re-applied.
public static class ModSys
{
    public static bool Fullscreen = true;
    static float nextCheck;
    static bool cutoutDone;

    public static void Tick()
    {
        if (Application.platform != RuntimePlatform.Android || Time.realtimeSinceStartup < nextCheck) return;
        nextCheck = Time.realtimeSinceStartup + 3f;
        try
        {
            using (var player = new AndroidJavaClass("com.unity3d.player.UnityPlayer"))
            {
                AndroidJavaObject activity = player.GetStatic<AndroidJavaObject>("currentActivity");
                activity.Call("runOnUiThread", new object[] { new AndroidJavaRunnable(ApplyOnUiThread) });
            }
        }
        catch (System.Exception e)
        {
            MonoBehaviour.print("[ravenmod] fullscreen failed: " + e.Message);
            nextCheck = float.MaxValue;
        }
    }

    static void ApplyOnUiThread()
    {
        using (var player = new AndroidJavaClass("com.unity3d.player.UnityPlayer"))
        {
            AndroidJavaObject activity = player.GetStatic<AndroidJavaObject>("currentActivity");
            AndroidJavaObject window = activity.Call<AndroidJavaObject>("getWindow");
            AndroidJavaObject decor = window.Call<AndroidJavaObject>("getDecorView");
            // IMMERSIVE_STICKY | LAYOUT_FULLSCREEN | LAYOUT_HIDE_NAVIGATION | LAYOUT_STABLE | FULLSCREEN | HIDE_NAVIGATION
            decor.Call("setSystemUiVisibility", new object[] { Fullscreen ? 0x1706 : 0 });
            if (!cutoutDone)
            {
                cutoutDone = true;
                AndroidJavaObject attrs = window.Call<AndroidJavaObject>("getAttributes");
                attrs.Set<int>("layoutInDisplayCutoutMode", 1);  // draw behind notches: SHORT_EDGES
                window.Call("setAttributes", new object[] { attrs });
            }
        }
    }
}

// Touch and gamepad always work. InputMode only follows the last device used, because game code
// branches on it, and whether the touch buttons show is a separate choice of the player.
// Portuguese text: an exact match first, then the fixed part of text built at runtime.
public static class ModLang
{
    public static bool On = true;
    public static bool Raw;  // text of the mod's own pages is already final
    static Dictionary<string, string> words;
    static Dictionary<string, string> upper;
    static List<string> pieces;
    static readonly Dictionary<string, string> cache = new Dictionary<string, string>();
    static readonly Dictionary<string, bool> missed = new Dictionary<string, bool>();

    static string Norm(string s)
    {
        var b = new System.Text.StringBuilder(s.Length);
        bool space = false;
        foreach (char c in s)
        {
            if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { space = true; continue; }
            if (space && b.Length > 0) b.Append(' ');
            space = false;
            b.Append(c);
        }
        return b.ToString();
    }

    static void Load()
    {
        words = new Dictionary<string, string>();
        upper = new Dictionary<string, string>();
        pieces = new List<string>();
        string[] p = ModData.Words.Split('\u0001');
        for (int i = 0; i + 1 < p.Length; i += 2)
        {
            string key = Norm(p[i]);
            if (key.Length == 0) continue;
            words[key] = p[i + 1].Trim();
            upper[key.ToUpper()] = p[i + 1].Trim();
            if (p[i] != p[i].Trim() || p[i].EndsWith(":") || p[i].EndsWith("(")) pieces.Add(p[i]);
        }
        pieces.Sort((a, b) => b.Length - a.Length);
    }

    public static string Fit(string s, TextMesh mesh)
    {
        if (Raw) return s;
        return T(s);
    }

    public static string T(string s)
    {
        if (!On || string.IsNullOrEmpty(s)) return s;
        if (words == null) Load();
        string r;
        if (cache.TryGetValue(s, out r)) return r;
        r = Compute(s);
        if (cache.Count > 8000) cache.Clear();
        cache[s] = r;
        return r;
    }

    static string Compute(string s)
    {
        string r = Whole(s);
        if (r != null) return r;
        if (s.IndexOf('\n') >= 0)
        {
            string[] lines = s.Split('\n');
            bool any = false;
            for (int i = 0; i < lines.Length; i++)
            {
                string l = Line(lines[i]);
                if (l != null) { lines[i] = l; any = true; }
            }
            if (any) return string.Join("\n", lines);
        }
        r = Line(s);
        if (r != null) return r;
        if (!missed.ContainsKey(s) && missed.Count < 2000 && HasLetters(s))
        {
            missed[s] = true;
            MonoBehaviour.print("[ravenlang] " + s);
        }
        return s;
    }

    static bool HasLetters(string s)
    {
        foreach (char c in s) if (c >= 'a' && c <= 'z') return true;
        return false;
    }

    // Keeps the spaces around the text: pieces of a longer text rely on them.
    static string Whole(string s)
    {
        string key = Norm(s), r;
        if (key.Length == 0) return s;
        int a = 0, b = s.Length;
        while (a < b && s[a] == ' ') a++;
        while (b > a && s[b - 1] == ' ') b--;
        string lead = s.Substring(0, a), trail = s.Substring(b);
        if (words.TryGetValue(key, out r)) return lead + r + trail;
        if (key == key.ToUpper() && upper.TryGetValue(key, out r)) return lead + r.ToUpper() + trail;
        return null;
    }

    static string Line(string s)
    {
        string r = Whole(s);
        if (r != null) return r;
        // Text followed by a value: "Level 5", "Armor: 12".
        int end = s.Length;
        while (end > 0 && (char.IsDigit(s[end - 1]) || s[end - 1] == '.' || s[end - 1] == ',' || s[end - 1] == '/' || s[end - 1] == '+' || s[end - 1] == '-' || s[end - 1] == '%')) end--;
        if (end > 0 && end < s.Length)
        {
            r = Whole(s.Substring(0, end));
            if (r != null) return r + s.Substring(end);
        }
        // A value followed by text: "2 minutes".
        int start = 0;
        while (start < s.Length && (char.IsDigit(s[start]) || s[start] == ' ')) start++;
        if (start > 0 && start < s.Length)
        {
            r = Line(s.Substring(start));
            if (r != null) return s.Substring(0, start) + r;
        }
        foreach (string p in pieces)
        {
            if (s.Length <= p.Length) continue;
            if (s.StartsWith(p))
            {
                string rest = s.Substring(p.Length);
                string tail = Line(rest);
                return Whole(p) + (tail ?? rest);
            }
            if (s.EndsWith(p))
            {
                string rest = s.Substring(0, s.Length - p.Length);
                string head = Whole(rest);
                return (head ?? rest) + Whole(p);
            }
        }
        return null;
    }
}

// Moves a highlight between whatever can be pressed on screen, so that every screen of the game
// works with a gamepad: menus, conversations, character creation, confirmations.
public static class ModNav
{
    public static bool Blocking;  // a menu has the sticks: the character must not move
    static SimpleGUIElement focus;
    static readonly List<SimpleGUIElement> cands = new List<SimpleGUIElement>();
    static readonly string[] NotMenu = { "BtnAttack", "BtnBlock", "BtnCameraMode", "BtnInventory", "BtnJump", "BtnMagic", "ButtonGameMenu",
        "VirtualStick", "BtnActivate", "CompassButton", "BackgroundBtn", "BackgroundFade", "BackgroundModal", "SwipeLook" };
    const BindingFlags Any = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
    static float nextMove;
    static int moveDir;
    static int frame;
    static string lastList = "";
    static float lastReal;
    static bool untouched;
    static bool sliding;
    static readonly Dictionary<SimpleGUIContainer, float> actionBase = new Dictionary<SimpleGUIContainer, float>();

    static void LogList()
    {
        var b = new System.Text.StringBuilder();
        foreach (SimpleGUIElement e in cands) { b.Append(e.gameObject.name); b.Append(' '); }
        string list = b.ToString();
        if (list == lastList) return;
        lastList = list;
        MonoBehaviour.print("[ravennav] " + cands.Count + ": " + (list.Length > 400 ? list.Substring(0, 400) : list));
    }
    static Texture2D gold, glow;

    static FieldInfo Gui(string name) { return typeof(SimpleGUI).GetField(name, Any); }

    static bool Usable(SimpleGUIElement e, bool modal, List<SimpleGUIElement> modalList)
    {
        if (e == null || !e.isEnabled) return false;
        var strip = e as SimpleGUIContainer;
        if (strip != null)
        {
            // Has no quad of its own: what shows is the selected item.
            if (!strip.enableSelection || strip.GetComponent<GuiInventory>() == null) return false;
            if (strip.getSelectionCount() == 0) return false;
            if (strip.getSelection() == null) strip.setSelection(0);
            if (strip.getSelection() == null) return false;
            if (strip.getSelection().gameObject.layer == MyGameMgr.LAYER_HIDDEN) return false;
            if (modal && !modalList.Contains(e)) return false;
            strip.getSelection().calcScreenRect();
            return true;
        }
        if (!(e is SimpleButton) && !(e is SimpleSlider)) return false;
        if (e.gameObject.layer == MyGameMgr.LAYER_HIDDEN || e.hiddenIfController) return false;
        if (modal && !modalList.Contains(e)) return false;
        if (System.Array.IndexOf(NotMenu, e.gameObject.name) >= 0) return false;
        e.calcScreenRect();
        Vector2 size = e.r.getSize(), low = e.r.getLowerLeft();
        float w = Screen.get_width(), h = Screen.get_height();
        if (size.x < 4f || size.y < 4f || size.x * size.y > 0.5f * w * h) return false;
        float cx = low.x + size.x * 0.5f, cy = low.y + size.y * 0.5f;
        if (cx < 0f || cx > w || cy < 0f || cy > h) return false;
        // Rows scrolled out of a list stay registered: only what shows inside its list counts.
        var box = e.getParent() as SimpleGUIContainer;
        if (box != null && box.scrollable)
        {
            box.calcScreenRect();
            Vector2 bl = box.r.getLowerLeft(), bs = box.r.getSize();
            if (bs.y > 4f && (cy < bl.y || cy > bl.y + bs.y)) return false;
        }
        return true;
    }

    static void Collect()
    {
        cands.Clear();
        if (SimpleGUI.instance == null) return;
        var events = Gui("listEventElements").GetValue(SimpleGUI.instance) as List<SimpleGUIElement>;
        var modalList = Gui("listElementsModal").GetValue(SimpleGUI.instance) as List<SimpleGUIElement>;
        bool modal = (bool)Gui("isModal").GetValue(SimpleGUI.instance);
        if (events == null) return;
        foreach (SimpleGUIElement e in events) if (Usable(e, modal, modalList)) cands.Add(e);
        // The Menu tab is the same object as the button on the HUD: it counts only among the tabs.
        bool tabs = false;
        foreach (SimpleGUIElement e in cands) if (e.gameObject.name == "ButtonInventory") tabs = true;
        if (!tabs) return;
        foreach (SimpleGUIElement e in events)
        {
            if (e.gameObject.name != "ButtonGameMenu" || !e.isEnabled || e.gameObject.layer == MyGameMgr.LAYER_HIDDEN) continue;
            e.calcScreenRect();
            cands.Add(e);
        }
    }

    static Vector2 Center(SimpleGUIElement e)
    {
        var strip = e as SimpleGUIContainer;
        if (strip != null && strip.getSelection() != null) e = strip.getSelection();
        Vector2 size = e.r.getSize(), low = e.r.getLowerLeft();
        return new Vector2(low.x + size.x * 0.5f, low.y + size.y * 0.5f);
    }

    static SimpleGUIElement First()
    {
        if (ModPage.IsOpen) return ModPage.FirstRow();
        // Where there are items, they are what the screen is for.
        foreach (SimpleGUIElement e in cands) if (e is SimpleGUIContainer) return e;
        SimpleGUIElement best = null;
        float bestScore = float.MinValue;
        foreach (SimpleGUIElement e in cands)
        {
            Vector2 c = Center(e);
            float score = c.y * 2f - c.x;  // top left first
            if (score > bestScore) { bestScore = score; best = e; }
        }
        return best;
    }

    static SimpleGUIElement Step(Vector2 d, bool wrap)
    {
        Vector2 from = Center(focus);
        SimpleGUIElement best = null;
        float bestScore = float.MaxValue;
        foreach (SimpleGUIElement e in cands)
        {
            if (e == focus) continue;
            Vector2 v = Center(e) - from;
            float along = v.x * d.x + v.y * d.y;
            float side = Mathf.Abs(v.x * d.y - v.y * d.x);
            if (!wrap && along < 8f) continue;
            float score = (wrap ? -along : along) + side * 2.5f;
            if (score < bestScore) { bestScore = score; best = e; }
        }
        return best;
    }

    // Long lists: a press past the last visible row moves the list instead.
    static bool ScrollList(float dy)
    {
        var box = focus.getParent() as SimpleGUIContainer;
        if (box == null || !box.scrollable) return false;
        object step = typeof(SimpleGUIElement).GetMethod("screenSizeToWorldSize", Any).Invoke(box, new object[] { new Vector2(0f, focus.r.getSize().y) });
        float amount = ((Vector2)step).y;
        Vector3 before = box.transform.localPosition;
        Vector3 wanted = before + new Vector3(0f, dy < 0f ? amount : -amount, 0f);
        Vector3 after = (Vector3)typeof(SimpleGUIContainer).GetMethod("clampScrollPos", Any).Invoke(box, new object[] { wanted });
        box.transform.localPosition = after;
        return Mathf.Abs(after.y - before.y) > 0.0001f;
    }

    public static void Forget()
    {
        focus = null;
    }

    public static void Tick(InputHandler h)
    {
        frame++;
        Blocking = false;
        Collect();
        LogList();
        if (cands.Count == 0) { focus = null; return; }
        Blocking = true;
        SeparateActions();
        if (focus == null || !cands.Contains(focus))
        {
            untouched = true;
            nextMove = Time.realtimeSinceStartup + 0.15f;
        }
        // Screens slide in: until the first move, the top entry is judged where it ended up.
        if (untouched) focus = First();
        if (focus == null) return;
        if (ModPage.IsOpen)
        {
            ModPage.Focused(focus);
            if (Input.GetButtonDown("Button1")) { ModPage.Back(); return; }
            if (Input.GetButtonDown("Button6")) { ModPage.Turn(-1); return; }
            if (Input.GetButtonDown("Button7")) { ModPage.Turn(1); return; }
        }
        var items = focus as SimpleGUIContainer;
        if (items != null && items.getSelectionIndex() < 0) MoveItems(items, true);

        var map = h.mapControllerInput;
        float x = map[InputHandler.ControllerInput.DpadX].getAxis();
        if (Mathf.Abs(x) < 0.5f) x = map[InputHandler.ControllerInput.LStickX].getAxis();
        float y = map[InputHandler.ControllerInput.DpadY].getAxis();
        if (Mathf.Abs(y) < 0.5f) y = map[InputHandler.ControllerInput.LStickY].getAxis();

        var slider = focus as SimpleSlider;
        // Pushed again once at its end, a slider lets the highlight go to whatever is beside it.
        bool atEnd = slider != null && !sliding && ((x > 0f && slider.elemFloat >= 0.999f) || (x < 0f && slider.elemFloat <= 0.001f));
        if (Mathf.Abs(x) < 0.2f) sliding = false;
        if (slider != null && Mathf.Abs(x) >= 0.2f && !atEnd)
        {
            sliding = true;
            slider.updateSlider(slider.elemFloat + x * Mathf.Min(Time.realtimeSinceStartup - lastReal, 0.1f) * 0.6f);
            var handlers = typeof(SimpleSlider).GetField("btnHandlers", Any).GetValue(slider) as List<SimpleEventHandler>;
            if (handlers != null) foreach (SimpleEventHandler handler in handlers) handler.onEvent(slider);
            x = 0f;
        }

        int dir = 0;
        Vector2 d = Vector2.zero;
        if (Mathf.Abs(y) >= 0.5f && Mathf.Abs(y) >= Mathf.Abs(x)) { d.y = y > 0f ? 1f : -1f; dir = y > 0f ? 1 : 2; }
        else if (Mathf.Abs(x) >= 0.5f) { d.x = x > 0f ? 1f : -1f; dir = x > 0f ? 3 : 4; }
        float now = Time.realtimeSinceStartup;
        if (dir == 0) moveDir = 0;
        else if (dir != moveDir || now >= nextMove)
        {
            bool held = dir == moveDir;
            untouched = false;
            moveDir = dir;
            nextMove = now + (held ? 0.14f : 0.38f);
            // Inside a strip of items its own axis walks the items; the other one leaves the strip.
            bool vertical = items != null && items.layoutMode == SimpleGUIContainer.LayoutMode.VERTICAL;
            if (items != null && (vertical ? d.y != 0f : d.x != 0f))
            {
                MoveItems(items, vertical ? d.y < 0f : d.x > 0f);
                lastReal = now;
                PressItem(items);
                return;
            }
            var row = focus.GetComponent<ModRowHandler>();
            if (row != null && d.x != 0f && ModPage.IsValueRow(row.Slot))
            {
                ModPage.Press(row.Slot, d.x > 0f ? 1 : -1);
                lastReal = now;
                return;
            }
            SimpleGUIElement next = Step(d, false);
            if (next == null && d.y != 0f && ScrollList(d.y)) next = focus;
            else if (next == null) next = Step(d, true);
            if (next != null) focus = next;
        }
        var button = focus as SimpleButton;
        if (button != null && Input.GetButtonDown("Button0")) button.simulatePress();
        if (items != null) PressItem(items);
        lastReal = Time.realtimeSinceStartup;
    }

    static void MoveItems(SimpleGUIContainer items, bool forward)
    {
        typeof(SimpleGUIContainer).GetMethod(forward ? "nextSelection" : "prevSelection", Any).Invoke(items, null);
        var chosen = items.getSelection();
        if (chosen == null) return;
        // The strip scrolls by hand here, so the chosen item has to be brought into view.
        chosen.calcScreenRect();
        float w = Screen.get_width(), c = Center(chosen).x;
        if (c > w * 0.12f && c < w * 0.88f) return;
        object step = typeof(SimpleGUIElement).GetMethod("screenSizeToWorldSize", Any).Invoke(items, new object[] { new Vector2(w * 0.5f - c, 0f) });
        Vector3 wanted = items.transform.localPosition + new Vector3(((Vector2)step).x, 0f, 0f);
        items.transform.localPosition = (Vector3)typeof(SimpleGUIContainer).GetMethod("clampScrollPos", Any).Invoke(items, new object[] { wanted });
    }

    // A on an item runs what the item offers first, never Drop: that one takes going to its button.
    static void PressItem(SimpleGUIContainer items)
    {
        if (!Input.GetButtonDown("Button0")) return;
        var inventory = items.GetComponent<GuiInventory>();
        if (inventory == null || inventory.actionContainer == null) return;
        foreach (SimpleGUIElement e in inventory.actionContainer.getChildren())
        {
            var b = e as SimpleButton;
            if (b == null || !b.isEnabled || b.elemData1 == "DropItem") continue;
            b.simulatePress();
            return;
        }
    }

    // The filters stay on screen with a pad, in the row the action buttons use: these go one above.
    static void SeparateActions()
    {
        foreach (SimpleGUIElement e in cands)
        {
            if (!(e is SimpleGUIContainer)) continue;
            var equip = e.GetComponent<GuiInventoryEquip>();
            if (equip == null || equip.actionContainer == null || equip.filterContainer == null) continue;
            var box = equip.actionContainer;
            if (box.getChildren().Count == 0) return;
            float raised;
            if (!actionBase.TryGetValue(box, out raised))
            {
                object one = typeof(SimpleGUIElement).GetMethod("screenSizeToWorldSize", Any).Invoke(box, new object[] { new Vector2(0f, Screen.get_height() * 0.112f) });
                raised = box.transform.position.y + ((Vector2)one).y;
                actionBase[box] = raised;
            }
            Vector3 p = box.transform.position;
            if (p.y != raised)
            {
                p.y = raised;
                box.transform.position = p;
            }
            return;
        }
    }

    static Texture2D Solid(Color c)
    {
        var t = new Texture2D(1, 1);
        t.SetPixel(0, 0, c);
        t.Apply();
        return t;
    }

    public static void Draw()
    {
        if (focus == null || InputHandler.inputMode != InputHandler.InputMode.CONTROLLER) return;
        if (gold == null)
        {
            gold = Solid(new Color(1f, 0.82f, 0.28f, 1f));
            glow = Solid(new Color(1f, 0.78f, 0.2f, 0.2f));
        }
        var m = new Matrix4x4();
        m.m00 = 1f; m.m11 = 1f; m.m22 = 1f; m.m33 = 1f;
        GUI.matrix = m;
        SimpleGUIElement shown = focus;
        var items = focus as SimpleGUIContainer;
        if (items != null && items.getSelection() != null)
        {
            shown = items.getSelection();
            shown.calcScreenRect();
        }
        Vector2 size = shown.r.getSize(), low = shown.r.getLowerLeft();
        float h = Screen.get_height();
        float t = 3f + 2f * Mathf.Abs(Mathf.Sin(Time.realtimeSinceStartup * 3f));
        float pad = 6f;
        float l = low.x - pad, top = h - low.y - size.y - pad, w = size.x + pad * 2f, ht = size.y + pad * 2f;
        GUI.DrawTexture(new Rect(l, top, w, ht), glow);
        GUI.DrawTexture(new Rect(l, top, w, t), gold);
        GUI.DrawTexture(new Rect(l, top + ht - t, w, t), gold);
        GUI.DrawTexture(new Rect(l, top, t, ht), gold);
        GUI.DrawTexture(new Rect(l + w - t, top, t, ht), gold);
    }
}

// The projected shadow is one texture around the player: this sets how far and how sharp it is.
public static class ModShadow
{
    const BindingFlags Any = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
    static readonly FieldInfo DistField = typeof(CharacterShadow).GetField("dShadow", Any);
    static readonly FieldInfo ProjField = typeof(CharacterShadow).GetField("proj", Any);
    static readonly FieldInfo ChildField = typeof(CharacterShadow).GetField("child", Any);
    static readonly FieldInfo MapField = typeof(CharacterShadow).GetField("shadowmap", Any);
    static readonly FieldInfo CenterField = typeof(CharacterShadow).GetField("center", Any);
    const float MaxSlope = 1.43f;  // tangent of 55 degrees
    static float applied;
    static ShadowMgr appliedTo;

    public static void Fit(CharacterShadow cs)
    {
        float range = ModCfg.ProjRange;
        var map = (RenderTexture)MapField.GetValue(cs);
        if (map != null && map.width != ModCfg.ProjSize)
        {
            Object.Destroy(map);
            MapField.SetValue(cs, null);  // LateUpdate makes a new one at the chosen size
        }
        // A new instance comes with each loaded scene.
        if ((range != applied || appliedTo != ShadowMgr.instance) && ShadowMgr.instance != null)
        {
            applied = range;
            appliedTo = ShadowMgr.instance;
            typeof(ShadowMgr).GetField("maxDistanceFullShadow", Any).SetValue(ShadowMgr.instance, range);
            typeof(ShadowMgr).GetField("maxDistanceSimpleShadow", Any).SetValue(ShadowMgr.instance, range * 1.5f);
        }
        var child = (GameObject)ChildField.GetValue(cs);
        var cam = child.camera;
        var proj = (Projector)ProjField.GetValue(cs);
        // The game's light is nearly overhead, which leaves each shadow hidden under the feet.
        // Same direction on the ground, lower in the sky, so that characters stand on something.
        Vector3 center = (Vector3)CenterField.GetValue(cs);
        Vector3 toLight = cs.transform.position - center;
        float flat = Mathf.Sqrt(toLight.x * toLight.x + toLight.z * toLight.z);
        float wanted = toLight.y / MaxSlope;
        if (flat > 1f && wanted > flat)
        {
            toLight.x *= wanted / flat;
            toLight.z *= wanted / flat;
        }
        child.transform.position = center + toLight;
        child.transform.LookAt(center);
        float dist = toLight.magnitude;
        // Stones and buildings are on a layer of their own: without it shadows stop at their edge.
        proj.ignoreLayers = ~((1 << MyGameMgr.LAYER_WORLD) | (1 << MyGameMgr.LAYER_WORLD_OBJECT));
        cam.orthographicSize = range;
        cam.nearClipPlane = Mathf.Max(dist - range, 1f);
        cam.farClipPlane = dist + range;
        proj.orthoGraphicSize = range;
        // Ground above and below the player has to receive the shadow too, or it ends in a cut.
        proj.nearClipPlane = Mathf.Max(dist - range * 0.5f, 1f);
        proj.farClipPlane = dist + cs.castingDistance + range * 0.5f;
    }
}

public static class ModInput
{
    public static int TouchUi;       // 0 follow the last device, 1 always shown, 2 always hidden
    public static bool HideTouch;    // read by the patched SimpleGUI.setElementLayer

    static Dictionary<string, List<string[]>> binds;
    static readonly Dictionary<string, FieldInfo> fieldCache = new Dictionary<string, FieldInfo>();
    static VirtualStick stick;
    static bool blockHeld, stealthHeld;
    static float stealthAt;

    static readonly string[] Hud = { "BtnAttack", "BtnBlock", "BtnCameraMode", "BtnInventory", "BtnJump", "BtnMagic", "ButtonGameMenu", "VirtualStick" };

    static void LoadBinds()
    {
        binds = new Dictionary<string, List<string[]>>();
        string[] p = ModData.Pad.Split('\u0001');
        for (int i = 0; i + 2 < p.Length; i += 3)
        {
            string name = p[i], field = p[i + 1], value = p[i + 2];
            if (field == "hiddenIfController")
            {
                if (System.Array.IndexOf(Hud, name) >= 0) AddBind(name, field, value);
                continue;
            }
            if (name == "BtnBlock") continue;  // a tap button in the touch scene: held in PadActions
            // Menus go through ModNav: A presses what is highlighted, the sticks move the highlight.
            if (field == "controlAxis" || field == "controlAxis2") continue;
            if (value == "12" && name != "TapCapture" && name != "BtnActivate") continue;
            AddBind(name, field, value);
        }
        AddBind("ButtonGameMenu", "controlBtnPress", "16");  // Start
    }

    static void AddBind(string name, string field, string value)
    {
        List<string[]> list;
        if (!binds.TryGetValue(name, out list)) binds[name] = list = new List<string[]>();
        foreach (string[] b in list) if (b[0] == field) return;
        list.Add(new[] { field, value });
    }

    public static void Bind(SimpleGUIElement e)
    {
        if (e == null) return;
        if (binds == null) LoadBinds();
        var vs = e as VirtualStick;
        if (vs != null) stick = vs;
        List<string[]> list;
        if (!binds.TryGetValue(e.gameObject.name, out list)) return;
        foreach (string[] b in list)
        {
            FieldInfo f = FieldOf(e.GetType(), b[0]);
            if (f == null) continue;
            int v = int.Parse(b[1]);
            if (f.FieldType == typeof(bool)) f.SetValue(e, v != 0);
            else if (f.FieldType == typeof(int)) f.SetValue(e, v);
            else f.SetValue(e, System.Enum.ToObject(f.FieldType, v));
        }
    }

    static FieldInfo FieldOf(System.Type t, string name)
    {
        string key = t.Name + "." + name;
        FieldInfo f;
        if (!fieldCache.TryGetValue(key, out f)) fieldCache[key] = f = t.GetField(name, Any);
        return f;
    }

    // Held buttons that the touch scene only has as tap buttons.
    // A touch only fires these buttons on release: the press from a pad has to look the same.
    public static SimpleButton.ButtonState PressState(SimplePushButton b)
    {
        return b.sendActivateEvent || b.isContinuous ? SimpleButton.ButtonState.BTN_ACTIVATED : SimpleButton.ButtonState.BTN_RELEASED;
    }

    // Touching one of these reaches the handlers as a press, and jumping acts on the press.
    public static SimpleButton.ButtonState InstantState()
    {
        return SimpleButton.ButtonState.BTN_ACTIVATED;
    }

    static SimplePushButton cameraButton;

    static void PadActions(InputHandler h)
    {
        var player = GameActorPlayer.instance;
        if (player == null) return;
        // The camera button tells a tap from a hold, so R3 goes through a whole tap.
        if (Input.GetButtonDown("Button14") && Time.timeScale > 0f)
        {
            if (cameraButton == null)
            {
                var go = GameObject.Find("BtnCameraMode");
                if (go != null) cameraButton = go.GetComponent<SimplePushButton>();
            }
            if (cameraButton != null)
            {
                cameraButton.processActivated(Vector2.zero, -1);
                cameraButton.processReleased(Vector2.zero, -1);
            }
        }
        bool block = h.mapControllerInput[InputHandler.ControllerInput.LTrigger].getAxisDiscrete() > 0;
        if (block != blockHeld)
        {
            blockHeld = block;
            player.onActionBlock(block ? SimpleButton.ButtonState.BTN_ACTIVATED : SimpleButton.ButtonState.BTN_RELEASED);
        }
        bool stealth = Input.GetButton("Button13");
        if (stealth && !stealthHeld && Time.time - stealthAt >= 1f && Time.timeScale > 0f)
        {
            stealthAt = Time.time;
            if (player.isSneaking) player.isSneaking = false;
            else if (!player.isRiding && !player.isInCombat())
            {
                if (player.checkDetected() == null) player.isSneaking = true;
                else MyGameMgr.getInstance().showMessage("Hide failed.");
            }
        }
        stealthHeld = stealth;
    }

    static readonly string[] Buttons = BuildButtons();
    static readonly string[] Axes = { "Horizontal", "Vertical", "Axis3", "Axis4", "Axis5", "Axis6" };
    static readonly float[] lastAxis = new float[6];
    const BindingFlags Any = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;
    static bool prepared;
    static bool shownHide;

    static string[] BuildButtons()
    {
        var names = new string[17];
        for (int i = 0; i < names.Length; i++) names[i] = "Button" + i;
        return names;
    }

    public static float Dz(float f)
    {
        float d = ModCfg.DeadZone, a = Mathf.Abs(f);
        if (a <= d) return 0f;
        return Mathf.Sign(f) * (a - d) / (1f - d);  // no jump at the edge of the dead zone
    }

    // Edges only: an axis resting at -1 on some pad must not hold the game in controller mode.
    static bool PadEvent()
    {
        bool hit = false;
        foreach (string b in Buttons) if (Input.GetButtonDown(b)) hit = true;
        for (int i = 0; i < Axes.Length; i++)
        {
            float v = Mathf.Abs(Input.GetAxisRaw(Axes[i]));
            if (v > 0.5f && lastAxis[i] <= 0.5f) hit = true;
            lastAxis[i] = v;
        }
        return hit;
    }

    // Replaces InputHandler.onUpdate for touch and gamepad; false leaves other modes to it.
    public static bool OnUpdate(InputHandler h)
    {
        var mode = InputHandler.inputMode;
        if (mode != InputHandler.InputMode.TOUCH && mode != InputHandler.InputMode.CONTROLLER) return false;
        if (!prepared)
        {
            prepared = true;
            if (h.mapControllerInput == null) typeof(InputHandler).GetMethod("initControllerInput", Any).Invoke(h, null);
            h.mapControllerInput[InputHandler.ControllerInput.BtnBack].inputName = "Button10";
            RefreshTouchButtons(true);
        }

        int count = Input.touchCount;
        bool began = false;
        for (int i = 0; i < count; i++) if (Input.GetTouch(i).phase == TouchPhase.Began) began = true;
        bool pad = PadEvent();
        if (began) mode = InputHandler.InputMode.TOUCH;
        else if (pad) mode = InputHandler.InputMode.CONTROLLER;
        if (mode != InputHandler.inputMode)
        {
            InputHandler.inputMode = mode;
            MonoBehaviour.print("[ravenmod] last device: " + mode);
            if (mode == InputHandler.InputMode.TOUCH)
            {
                // processController stops running: nothing may stay held from the pad.
                if (stick != null && !stick.isGrabbed) { stick.power = 0f; stick.vDir = Vector2.zero; }
                if (blockHeld && GameActorPlayer.instance != null) GameActorPlayer.instance.onActionBlock(SimpleButton.ButtonState.BTN_RELEASED);
                blockHeld = false;
            }
        }
        RefreshTouchButtons(false);

        h.clearConsumedFlags();
        for (int i = 0; i < count; i++)
        {
            Touch t = Input.GetTouch(i);
            h.resetEventBoundsHandler();
            foreach (var handler in h.listTouchHandlers) handler(t.position, t.phase, t.fingerId, t.tapCount, t.deltaPosition);
        }
        if (mode == InputHandler.InputMode.TOUCH)
        {
            h.processTouchSpecialHandler();
        }
        else if (h.mapControllerInput != null)
        {
            foreach (var item in h.mapControllerInput) item.Value.resetConsume();
            h.processControllerHandler();
            ModNav.Tick(h);
            if (ModNav.Blocking && stick != null) { stick.power = 0f; stick.vDir = Vector2.zero; }
            if (!ModNav.Blocking) PadActions(h);
        }
        return true;
    }

    static void RefreshTouchButtons(bool force)
    {
        bool hide = TouchUi == 2 || (TouchUi == 0 && InputHandler.inputMode == InputHandler.InputMode.CONTROLLER);
        HideTouch = hide;
        if ((!force && hide == shownHide) || SimpleGUI.instance == null) return;
        shownHide = hide;
        var elements = typeof(SimpleGUI).GetField("listAllElements", Any).GetValue(SimpleGUI.instance) as List<SimpleGUIElement>;
        MethodInfo setLayer = typeof(SimpleGUI).GetMethod("setElementLayer", Any);
        if (elements == null) return;
        foreach (SimpleGUIElement e in elements)
        {
            if (e != null) setLayer.Invoke(SimpleGUI.instance, new object[] { e });
        }
    }
}

// Talks to the remaster pipeline in the native bridge, and tells it where the world camera and
// the sun are. Values travel as integers, ten-thousandths for fractions.
public static class ModFx
{
    [DllImport("libravenfx")] static extern void raven_fx_set(int key, int value);
    [DllImport("libravenfx")] static extern void raven_fx_mark();

    static Camera best;
    static Light sun;
    static int tick;
    static string lastCams = "";
    static string lastLights = "";
    static int nextSunScan;

    static void F(int key, float v) { raven_fx_set(key, (int)(v * 10000f)); }
    static float shadowWanted;
    public static int PacedFps;

    public static void Apply(bool on, int scale, int msaa, int aniso, float bloom, float ao, float shadow, float rays, float tone, float sat, float sharp, float vig, float exposure, float contrast, float range)
    {
        raven_fx_set(0, on ? 1 : 0);
        raven_fx_set(1, scale);
        raven_fx_set(2, msaa);
        raven_fx_set(11, aniso);
        F(3, bloom); F(4, ao); shadowWanted = shadow; F(6, rays); F(7, tone); F(8, sat); F(9, sharp); F(10, vig); F(12, exposure); F(13, contrast); F(14, range);
    }

    // On the script attached to world cameras.
    public static void OnPost(MonoBehaviour b)
    {
        if (best != null && b.camera == best) raven_fx_mark();
    }

    static void Scan()
    {
        Camera[] cams = Camera.allCameras;
        Camera chosen = null;
        string text = "";
        int world = (1 << 0) | (1 << MyGameMgr.LAYER_WORLD_OBJECT);
        foreach (Camera c in cams)
        {
            text += c.name + "(m" + c.cullingMask.ToString("x") + ") ";
            if ((c.cullingMask & world) != 0) chosen = c;  // the engine keeps them sorted by depth
        }
        best = chosen;
        if (best != null && best.GetComponent(typeof(ModelEyes)) == null) best.gameObject.AddComponent(typeof(ModelEyes));
        if (sun == null && tick >= nextSunScan)
        {
            nextSunScan = tick + 300;
            string lights = "";
            foreach (Object o in Object.FindObjectsOfType(typeof(Light)))
            {
                var l = (Light)o;
                if (l.type != LightType.Directional) continue;
                lights += l.name + (l.enabled ? " " : "(off) ");
                // Menus keep their own lights for the character preview.
                string n = l.name;
                if (l.enabled && n.IndexOf("Inventory") < 0 && n.IndexOf("Equip") < 0 && n.IndexOf("Gui") < 0 && n.IndexOf("Menu") < 0 && n.IndexOf("WorldMap") < 0) sun = l;
            }
            if (lights != lastLights)
            {
                lastLights = lights;
                MonoBehaviour.print("[ravenmod] directional lights: " + lights);
            }
        }
        if (text != lastCams)
        {
            lastCams = text;
            MonoBehaviour.print("[ravenmod] cameras: " + text + " world=" + (best != null ? best.name : "none") + " sun=" + (sun != null ? sun.name : "none"));
        }
    }

    public static void Tick()
    {
        raven_fx_set(40, PacedFps);
        if (tick++ % 30 == 0 || best == null) Scan();
        if (best == null) return;
        Transform t = best.transform;
        float tan = Mathf.Tan(best.fieldOfView * 0.5f * Mathf.Deg2Rad);
        float py = 1f / tan;
        F(20, best.nearClipPlane);
        F(21, best.farClipPlane);
        F(22, py / best.aspect);
        F(23, py);
        // Without a real sun any direction would contradict the light painted into the scene.
        if (sun == null)
        {
            F(27, 0f);
            F(30, 0f);
            return;
        }
        // A "Baked" light only shades normals: the scene's real shadows are painted in, and tracing
        // against it draws hard dark bands across walls that have nothing to do with them.
        F(5, sun.name.IndexOf("Baked") >= 0 ? 0f : shadowWanted);
        Vector3 d = -sun.transform.forward;  // towards the sun
        F(24, Vector3.Dot(d, t.right));
        F(25, Vector3.Dot(d, t.up));
        F(26, Vector3.Dot(d, t.forward));
        F(27, 1f);
        Vector3 v = best.WorldToViewportPoint(t.position + d * 1000f);
        F(28, v.x);
        F(29, v.y);
        float edge = Mathf.Max(Mathf.Abs(v.x - 0.5f), Mathf.Abs(v.y - 0.5f));
        F(30, v.z > 0f ? Mathf.Clamp01((1.2f - edge) / 0.5f) : 0f);
    }
}

// The settings panel, drawn with Unity's immediate GUI over any menu.
public static class ModPanel
{
    public static bool IsOpen;  // read by the patched InputHandler.onUpdate; the options live in the game's own menu now

    public static void OnTag(string tag) { }

    static int frames;
    static float nextFpsLog;

    // Frames really produced by the game, independent of what the system composes.
    static void LogFps()
    {
        frames++;
        float now = Time.realtimeSinceStartup;
        if (now < nextFpsLog) return;
        if (nextFpsLog > 0f) MonoBehaviour.print("[ravenmod] fps=" + (frames / 5f) + " target=" + Application.targetFrameRate);
        frames = 0;
        nextFpsLog = now + 5f;
    }

    public static void Tick()
    {
        ModCfg.EnsureBooted();
        LogFps();
        ModSys.Tick();
        ModFx.Tick();
        ModPage.Ensure();
    }

    public static void OnGUI()
    {
        if (ModCfg.Opts == null) return;
        ModNav.Draw();
    }
}

public class ModRowHandler : SimpleEventHandler
{
    public int Slot;
    public override void onEvent(SimpleGUIElement elem) { ModPage.Press(Slot, 1); }
}

// The graphics options as pages of the game's own options screen, made of copies of its buttons.
public static class ModPage
{
    public static bool IsOpen;
    const string NoPage = "GuiModPage";  // no element has this tag: asking for it hides them all
    const int PerPage = 7;
    const int SlotBack = 100, SlotNext = 101, SlotPrev = 102, SlotGraphics = 103, SlotGame = 104;
    const BindingFlags Any = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;

    static SimplePushButton entryGraphics, entryGame, back, next, prev;
    static readonly SimplePushButton[] rows = new SimplePushButton[PerPage];
    static SimpleGUIElement footer, background, titleBar;
    static SimpleText titleText;
    static List<object> items = new List<object>();
    static string title = "";
    static int kind, page;
    static int footerSlot = -1;
    static float nextTry;

    // The game builds a quad only on objects without a mesh, and a copy would share the original's.
    static void NewMesh(GameObject go)
    {
        Object.DestroyImmediate(go.GetComponent<MeshRenderer>());
        Object.DestroyImmediate(go.GetComponent<MeshFilter>());
    }

    // A copied label brings the shadow text the game made for it, which nothing would ever hide.
    static void NewLabel(SimpleGUIElement copy, SimpleGUIElement src)
    {
        var old = new List<GameObject>();
        foreach (Transform child in copy.transform) old.Add(child.gameObject);
        foreach (GameObject child in old) Object.DestroyImmediate(child);
        copy.textLabel = SimpleText.create(" ", true, src.textLabel.textStyle);
    }

    static SimplePushButton Copy(SimplePushButton src, int slot, bool ownPage, float width, Vector2 pos)
    {
        var go = (GameObject)Object.Instantiate(src.gameObject);
        go.name = "ModRow" + slot;
        go.transform.parent = src.transform.parent != null ? src.transform.parent : SimpleGUI.instance.transform;
        Object.DestroyImmediate(go.GetComponent<BtnCustomControls>());
        NewMesh(go);
        var handler = go.AddComponent<ModRowHandler>();
        handler.Slot = slot;
        var b = go.GetComponent<SimplePushButton>();
        NewLabel(b, src);
        b.objHandler = null;
        b.sendActivateEvent = false;
        b.isContinuous = false;
        b.customPos43 = false;
        b.customPos169 = false;
        b.hiddenIfController = false;
        b.effectShow = null;
        b.effectHide = null;
        b.anchorPoint = SimpleGUIElement.AnchorPoint.CENTER;
        b.init();
        // Copies carry the size already adjusted to the screen: init adjusted it a second time.
        var srcSize = src.getWorldSize();
        b.setSize(new Vector2(srcSize.x * width, srcSize.y));
        var handlers = typeof(SimplePushButton).GetField("btnHandlers", Any).GetValue(b) as List<SimpleEventHandler>;
        handlers.Clear();
        handlers.Add(handler);
        b.setPositionGuiRelative(pos);
        if (ownPage) go.tag = "Untagged";
        if (ownPage || !src.isEnabled) b.hide(true);
        return b;
    }

    // Runs every frame: the buttons have to be rebuilt after each scene that replaces the menus.
    public static void Ensure()
    {
        if (entryGraphics != null || SimpleGUI.instance == null) return;
        float now = Time.realtimeSinceStartup;
        if (now < nextTry) return;
        nextTry = now + 1f;
        IsOpen = false;
        var src = GameObject.Find("BtnCustomControls");
        if (src == null) return;
        var button = src.GetComponent<SimplePushButton>();
        if (button == null || !(bool)typeof(SimpleGUIElement).GetField("initialized", Any).GetValue(button)) return;
        var bg = GameObject.Find("BackgroundOptions");
        var bar = GameObject.Find("TitleBar");
        background = bg != null ? bg.GetComponent<SimpleGUIElement>() : null;
        titleBar = bar != null ? bar.GetComponent<SimpleGUIElement>() : null;
        titleText = bar != null ? bar.GetComponentInChildren<SimpleText>() : null;

        for (int i = 0; i < PerPage; i++) rows[i] = Copy(button, i, true, 1.6f, new Vector2(0.5f, 0.19f + i * 0.105f));
        back = Copy(button, SlotBack, true, 0.7f, new Vector2(0.12f, 0.07f));
        next = Copy(button, SlotNext, true, 0.7f, new Vector2(0.88f, 0.93f));
        prev = Copy(button, SlotPrev, true, 0.7f, new Vector2(0.12f, 0.93f));
        var label = GameObject.Find("Difficulty Label");
        if (label != null)
        {
            var go = (GameObject)Object.Instantiate(label);
            go.name = "ModNote";
            go.transform.parent = button.transform.parent != null ? button.transform.parent : SimpleGUI.instance.transform;
            go.tag = "Untagged";
            NewMesh(go);
            footer = go.GetComponent<SimpleGUIElement>();
            NewLabel(footer, label.GetComponent<SimpleGUIElement>());
            footer.customPos43 = false;
            footer.customPos169 = false;
            footer.effectShow = null;
            footer.effectHide = null;
            footer.anchorPoint = SimpleGUIElement.AnchorPoint.CENTER;
            footer.init();
            footer.setPositionGuiRelative(new Vector2(0.5f, 0.93f));
            footer.hide(true);
        }
        entryGraphics = Copy(button, SlotGraphics, false, 1f, new Vector2(0.82f, 0.42f));
        entryGame = Copy(button, SlotGame, false, 1f, new Vector2(0.82f, 0.56f));
        Raw(entryGraphics, "Gráficos");
        Raw(entryGame, "Controles e Idioma");
    }

    static void Raw(SimpleGUIElement e, string s)
    {
        ModLang.Raw = true;
        e.setLabelText(s);
        ModLang.Raw = false;
    }

    static void Open(int newKind)
    {
        kind = newKind;
        page = 0;
        items.Clear();
        if (kind == 0)
        {
            title = "Gráficos";
            items.Add(ModCfg.Find("preset"));
            items.Add(ModCfg.Find("fps"));
            items.Add(ModCfg.Find("res5"));
            items.Add("advanced");
        }
        else if (kind == 1)
        {
            title = "Avançado";
            foreach (ModOpt o in ModCfg.Opts) if (ModCfg.IsGraphics(o)) items.Add(o);
        }
        else
        {
            title = "Controles e Idioma";
            items.Add(ModCfg.Find("lang"));
            items.Add(ModCfg.Find("touchui"));
            items.Add(ModCfg.Find("dead"));
            items.Add(ModCfg.Find("full"));
        }
        Show();
    }

    static int Pages() { return (items.Count + PerPage - 1) / PerPage; }

    static void Show()
    {
        IsOpen = true;
        SimpleGUI.instance.showOnlyElementsWithTag(NoPage, true);
        if (background != null) background.show(true);
        if (titleBar != null) titleBar.show(true);
        for (int i = 0; i < PerPage; i++) if (Item(i) != null) rows[i].show(true);
        back.show(true);
        if (Pages() > 1)
        {
            prev.show(true);
            next.show(true);
        }
        Raw(back, "Voltar");
        Raw(prev, "Anterior");
        Raw(next, "Próxima");
        footerSlot = -1;
        if (footer != null) footer.show(true);
        Refresh();
        ModNav.Forget();
    }

    static object Item(int slot)
    {
        int i = page * PerPage + slot;
        return i < items.Count ? items[i] : null;
    }

    static void Refresh()
    {
        ModLang.Raw = true;
        if (titleText != null) titleText.setText(Pages() > 1 ? title + "  " + (page + 1) + "/" + Pages() : title);
        ModLang.Raw = false;
        for (int i = 0; i < PerPage; i++)
        {
            object item = Item(i);
            var o = item as ModOpt;
            if (o != null) Raw(rows[i], "<   " + o.Label + ":  " + o.Shown + "   >");
            else if (item != null) Raw(rows[i], "Avançado");
        }
    }

    public static bool IsValueRow(int slot) { return slot < PerPage && Item(slot) is ModOpt; }

    public static SimpleGUIElement FirstRow() { return rows[0]; }

    public static void Press(int slot, int dir)
    {
        if (slot == SlotBack) Back();
        else if (slot == SlotGraphics) Open(0);
        else if (slot == SlotGame) Open(2);
        else if (slot == SlotNext || slot == SlotPrev) Turn(slot == SlotNext ? 1 : -1);
        else
        {
            object item = Item(slot);
            var o = item as ModOpt;
            if (o != null) Change(o, dir);
            else if (item != null) Open(1);
        }
    }

    public static void Turn(int dir)
    {
        if (Pages() < 2) return;
        page = (page + dir + Pages()) % Pages();
        Show();
    }

    static void Change(ModOpt o, int dir)
    {
        ModCfg.Change(o, dir);
        ModCfg.Save();
        ModCfg.Apply();
        Refresh();
        footerSlot = -1;
    }

    public static void Back()
    {
        if (kind == 1)
        {
            Open(0);
            return;
        }
        IsOpen = false;
        ModCfg.Save();
        ModCfg.Apply();
        if (titleText != null) titleText.setText("Options");
        SimpleGUI.instance.showOnlyElementsWithTag("GuiMenuOptions", true);
        ModNav.Forget();
    }

    // Shows what the option under the highlight is about.
    public static void Focused(SimpleGUIElement e)
    {
        if (footer == null) return;
        var h = e != null ? e.GetComponent<ModRowHandler>() : null;
        int slot = h != null ? h.Slot : -1;
        if (slot == footerSlot) return;
        footerSlot = slot;
        var o = slot >= 0 && slot < PerPage ? Item(slot) as ModOpt : null;
        Raw(footer, o != null && o.Note != null ? Wrap(o.Note) : " ");
    }

    static string Wrap(string s)
    {
        if (s.Length <= 44) return s;
        int cut = s.LastIndexOf(' ', 44);
        return cut > 0 ? s.Substring(0, cut) + "\n" + s.Substring(cut + 1) : s;
    }
}

