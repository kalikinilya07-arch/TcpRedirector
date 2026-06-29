using System.Collections.Specialized;
using System.Windows;
using System.Windows.Media;

namespace TcpRedirectorGUI.Adapters.Driving.Wpf.Controls;

/// <summary>
/// Lightweight WPF control that renders a real-time RX/TX traffic chart
/// using DrawingContext (no external charting library required).
/// Subscribes to INotifyCollectionChanged to auto-refresh when points are added.
/// </summary>
public class TrafficGraph : FrameworkElement
{
    // ── Data ──────────────────────────────────────────
    public static readonly DependencyProperty RxDataProperty =
        DependencyProperty.Register(nameof(RxData), typeof(IList<TrafficPoint>),
            typeof(TrafficGraph), new FrameworkPropertyMetadata(null,
                FrameworkPropertyMetadataOptions.AffectsRender, OnDataChanged));

    public static readonly DependencyProperty TxDataProperty =
        DependencyProperty.Register(nameof(TxData), typeof(IList<TrafficPoint>),
            typeof(TrafficGraph), new FrameworkPropertyMetadata(null,
                FrameworkPropertyMetadataOptions.AffectsRender, OnDataChanged));

    public IList<TrafficPoint>? RxData
    {
        get => (IList<TrafficPoint>?)GetValue(RxDataProperty);
        set => SetValue(RxDataProperty, value);
    }

    public IList<TrafficPoint>? TxData
    {
        get => (IList<TrafficPoint>?)GetValue(TxDataProperty);
        set => SetValue(TxDataProperty, value);
    }

    /// <summary>
    /// When RxData/TxData changes, subscribe to CollectionChanged
    /// so InvalidateVisual is called automatically on every add/remove.
    /// </summary>
    private static void OnDataChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var ctrl = (TrafficGraph)d;
        if (e.OldValue is INotifyCollectionChanged oldColl)
            oldColl.CollectionChanged -= ctrl.OnCollectionChanged;
        if (e.NewValue is INotifyCollectionChanged newColl)
            newColl.CollectionChanged += ctrl.OnCollectionChanged;
    }

    private void OnCollectionChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        InvalidateVisual();
    }

    // ── Drawing ───────────────────────────────────────
    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        var w = ActualWidth;
        var h = ActualHeight;

        if (w <= 0 || h <= 0) return;

        // Background
        dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(0x1E, 0x1E, 0x1E)), null, new Rect(0, 0, w, h));

        // Border
        var borderPen = new Pen(new SolidColorBrush(Color.FromRgb(0x3C, 0x3C, 0x3C)), 1);
        dc.DrawRectangle(null, borderPen, new Rect(0, 0, w, h));

        // Grid lines (horizontal — 4 lines)
        var gridPen = new Pen(new SolidColorBrush(Color.FromRgb(0x2D, 0x2D, 0x2D)), 0.5);
        for (int i = 1; i <= 3; i++)
        {
            var y = h * i / 4;
            dc.DrawLine(gridPen, new Point(0, y), new Point(w, y));
        }

        // Axis labels
        var labelBrush = new SolidColorBrush(Color.FromRgb(0x85, 0x85, 0x85));
        var typeface = new Typeface("Segoe UI");
        var fontSize = 10;

        var maxVal = ComputeMax(RxData, TxData);
        if (maxVal <= 0) maxVal = 1;

        // Y-axis labels
        for (int i = 0; i <= 4; i++)
        {
            var val = maxVal * (ulong)(4 - i) / 4;
            var label = FormatByteRate(val);
            var ft = new FormattedText(label, System.Globalization.CultureInfo.CurrentCulture,
                FlowDirection.LeftToRight, typeface, fontSize, labelBrush, 96);
            dc.DrawText(ft, new Point(4, h * i / 4 - ft.Height / 2));
        }

        // X-axis labels (last 60s)
        for (int i = 0; i <= 4; i++)
        {
            var sec = i * 15;
            var ft = new FormattedText($"-{60 - sec}s", System.Globalization.CultureInfo.CurrentCulture,
                FlowDirection.LeftToRight, typeface, fontSize, labelBrush, 96);
            dc.DrawText(ft, new Point(w - 60 + sec * (w - 50) / 60 - ft.Width / 2, h - ft.Height - 2));
        }

        // Draw data lines
        var margin = 45.0; // left margin for Y labels
        var plotW = w - margin - 8;
        var plotH = h - 20;

        DrawLine(dc, RxData, maxVal, plotW, plotH, margin, Color.FromRgb(0x4E, 0xC9, 0xB0)); // green
        DrawLine(dc, TxData, maxVal, plotW, plotH, margin, Color.FromRgb(0xCE, 0x91, 0x78)); // orange

        // Legend
        DrawLegend(dc, w);
    }

    private static void DrawLine(DrawingContext dc, IList<TrafficPoint>? data,
        ulong maxVal, double plotW, double plotH, double margin, Color color)
    {
        if (data == null || data.Count < 2 || maxVal == 0) return;

        var pen = new Pen(new SolidColorBrush(color), 1.5);
        var points = new List<Point>();

        // Normalize time: last 60 seconds
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var tStart = now - 60_000;

        foreach (var pt in data)
        {
            if (pt.Timestamp < tStart) continue;
            var x = margin + plotW * (pt.Timestamp - tStart) / 60_000.0;
            var y = plotH - plotH * pt.BytesPerSec / (double)maxVal;
            points.Add(new Point(x, y));
        }

        if (points.Count < 2) return;

        for (int i = 1; i < points.Count; i++)
            dc.DrawLine(pen, points[i - 1], points[i]);
    }

    private static void DrawLegend(DrawingContext dc, double w)
    {
        var typeface = new Typeface("Segoe UI");
        var labelBrush = new SolidColorBrush(Color.FromRgb(0xCC, 0xCC, 0xCC));

        var rxFt = new FormattedText("RX", System.Globalization.CultureInfo.CurrentCulture,
            FlowDirection.LeftToRight, typeface, 11, new SolidColorBrush(Color.FromRgb(0x4E, 0xC9, 0xB0)), 96);
        var txFt = new FormattedText("TX", System.Globalization.CultureInfo.CurrentCulture,
            FlowDirection.LeftToRight, typeface, 11, new SolidColorBrush(Color.FromRgb(0xCE, 0x91, 0x78)), 96);

        dc.DrawText(rxFt, new Point(w - 80, 4));
        dc.DrawText(txFt, new Point(w - 40, 4));
    }

    private static ulong ComputeMax(IList<TrafficPoint>? rx, IList<TrafficPoint>? tx)
    {
        ulong max = 0;
        if (rx != null)
            foreach (var p in rx)
                if (p.BytesPerSec > max) max = p.BytesPerSec;
        if (tx != null)
            foreach (var p in tx)
                if (p.BytesPerSec > max) max = p.BytesPerSec;

        // Round up to nice number
        if (max == 0) return 1024;
        ulong mag = 1;
        while (max >= 1000) { max /= 10; mag *= 10; }
        return (max + 1) * mag;
    }

    private static string FormatByteRate(ulong bps)
    {
        if (bps >= 1_000_000) return $"{bps / 1_000_000.0:F1} MB/s";
        if (bps >= 1_000) return $"{bps / 1_000.0:F0} KB/s";
        return $"{bps} B/s";
    }
}

/// <summary>
/// Data point for the traffic chart.
/// </summary>
/// <param name="Timestamp">Unix milliseconds</param>
/// <param name="BytesPerSec">Bytes per second at this point</param>
public record struct TrafficPoint(long Timestamp, ulong BytesPerSec);