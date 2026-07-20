using System.ComponentModel;
using System.Windows;

namespace TcpRedirectorGUI.Infrastructure.Localization;

/// <summary>
/// (Задача №1) Поддерживаемые языки интерфейса.
/// </summary>
public enum AppLanguage
{
    Russian,
    English
}

/// <summary>
/// (Задача №1) Служба локализации GUI. Реализует переключение языка «на лету»
/// (без перезапуска приложения) двумя механизмами:
///
///  1. <b>Статические строки XAML</b> — через merged <see cref="ResourceDictionary"/>
///     (<c>Strings.ru.xaml</c> / <c>Strings.en.xaml</c>). При смене языка нужный
///     словарь подменяется в <see cref="Application.Resources"/>, а XAML,
///     ссылающийся на строки через <c>{DynamicResource ...}</c>, обновляется
///     автоматически.
///
///  2. <b>Динамические строки ViewModel</b> — через статический
///     <see cref="Loc"/>-хелпер (<c>Loc.T("Key")</c>), читающий текущий словарь.
///     ViewModel'и, желающие обновиться при смене языка, подписываются на
///     <see cref="Loc.Instance"/> (реализует <see cref="INotifyPropertyChanged"/>).
///
/// Выбор языка персистится вызывающей стороной (ShellViewModel) в config.json
/// под GUI-ключом <c>gui.language</c>.
/// </summary>
public sealed class LocalizationService
{
    private const string RuDictUri =
        "Adapters/Driving/Wpf/Localization/Strings.ru.xaml";
    private const string EnDictUri =
        "Adapters/Driving/Wpf/Localization/Strings.en.xaml";

    private ResourceDictionary? _current;

    public AppLanguage CurrentLanguage { get; private set; } = AppLanguage.Russian;

    /// <summary>Событие смены языка (для обновления динамических строк VM).</summary>
    public event Action<AppLanguage>? LanguageChanged;

    /// <summary>Разбор строкового кода языка (из config.json). Дефолт — RU.</summary>
    public static AppLanguage ParseLanguage(string? code) =>
        (code?.Trim().ToLowerInvariant()) switch
        {
            "en" => AppLanguage.English,
            _ => AppLanguage.Russian
        };

    /// <summary>Код языка для сохранения в config.json.</summary>
    public static string ToCode(AppLanguage lang) =>
        lang == AppLanguage.English ? "en" : "ru";

    /// <summary>
    /// Применяет язык: подменяет merged-словарь строк и уведомляет подписчиков.
    /// Идемпотентно относительно уже применённого языка (словарь всё равно
    /// пересобирается, чтобы гарантировать корректное состояние при старте).
    /// </summary>
    public void SetLanguage(AppLanguage lang)
    {
        var app = Application.Current;
        if (app is null) return;

        var dictUri = lang == AppLanguage.English ? EnDictUri : RuDictUri;
        var newDict = new ResourceDictionary
        {
            Source = new Uri(dictUri, UriKind.Relative)
        };

        // Удаляем предыдущий словарь строк (если был), добавляем новый.
        if (_current is not null)
            app.Resources.MergedDictionaries.Remove(_current);
        app.Resources.MergedDictionaries.Add(newDict);
        _current = newDict;

        CurrentLanguage = lang;

        // Обновляем статический хелпер, затем уведомляем VM.
        Loc.SetDictionary(newDict);
        LanguageChanged?.Invoke(lang);
    }
}

/// <summary>
/// (Задача №1) Статический доступ к локализованным строкам из кода (ViewModel).
/// Также реализует <see cref="INotifyPropertyChanged"/> через индексатор, чтобы
/// XAML-биндинги вида <c>{Binding Path=[Key], Source={x:Static loc:Loc.Instance}}</c>
/// (при необходимости) обновлялись при смене языка. Для большинства статических
/// строк предпочтителен <c>{DynamicResource ...}</c>.
/// </summary>
public sealed class Loc : INotifyPropertyChanged
{
    public static Loc Instance { get; } = new();

    private static ResourceDictionary? s_dict;

    private Loc() { }

    /// <summary>Вызывается службой при смене языка.</summary>
    internal static void SetDictionary(ResourceDictionary dict)
    {
        s_dict = dict;
        // Индексатор целиком «поменялся» — уведомляем биндинги.
        Instance.PropertyChanged?.Invoke(Instance,
            new PropertyChangedEventArgs("Item[]"));
    }

    /// <summary>Локализованная строка по ключу; ключ возвращается как есть, если не найден.</summary>
    public static string T(string key)
    {
        if (s_dict is not null && s_dict.Contains(key) && s_dict[key] is string s)
            return s;
        // Фоллбэк на глобальные ресурсы приложения (на случай раннего вызова).
        var app = Application.Current;
        if (app is not null && app.Resources.Contains(key) && app.Resources[key] is string gs)
            return gs;
        return key;
    }

    /// <summary>Индексатор для XAML-биндингов (обновляется при смене языка).</summary>
    public string this[string key] => T(key);

    public event PropertyChangedEventHandler? PropertyChanged;
}
