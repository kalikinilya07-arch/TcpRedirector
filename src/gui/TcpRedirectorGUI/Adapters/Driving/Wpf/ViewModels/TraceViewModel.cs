using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using TcpRedirectorGUI.Domain.Entities;

namespace TcpRedirectorGUI.Adapters.Driving.Wpf.ViewModels;

/// <summary>
/// Live packet/connection trace: the set of TCP connections the service is
/// currently redirecting, refreshed from <c>get_connections</c> on the polling
/// timer. Rows are merged in place (by <see cref="ConnectionRecord.Id"/>) so the
/// grid updates smoothly instead of flickering on every poll.
/// </summary>
public partial class TraceViewModel : ObservableObject
{
    /// <summary>Currently active redirected connections.</summary>
    public ObservableCollection<ConnectionRecord> Connections { get; } = new();

    [ObservableProperty] private int _count;

    /// <summary>Informational note shown below the trace (e.g. mode limitations).</summary>
    [ObservableProperty] private string _note = "";

    /// <summary>
    /// Replace the trace contents with <paramref name="records"/>, merging in
    /// place so existing rows keep their identity. Marshals to the UI thread.
    /// </summary>
    public void PushConnections(IReadOnlyList<ConnectionRecord> records)
    {
        _ = App.Current.Dispatcher.BeginInvoke(() =>
        {
            // Remove rows no longer present.
            var liveIds = new HashSet<ulong>(records.Select(r => r.Id));
            for (int i = Connections.Count - 1; i >= 0; i--)
            {
                if (!liveIds.Contains(Connections[i].Id))
                    Connections.RemoveAt(i);
            }

            // Add/update rows from the fresh snapshot. ConnectionRecord is an
            // immutable-ish POCO (no INPC), so replace-in-place by index to make
            // the grid re-render the changed row while preserving row order.
            foreach (var rec in records)
            {
                var idx = -1;
                for (int i = 0; i < Connections.Count; i++)
                {
                    if (Connections[i].Id == rec.Id) { idx = i; break; }
                }

                if (idx < 0)
                    Connections.Add(rec);
                else
                    Connections[idx] = rec;
            }

            Count = Connections.Count;
        });
    }

    /// <summary>Clear the trace (e.g. on disconnect / service stop).</summary>
    public void Clear()
    {
        _ = App.Current.Dispatcher.BeginInvoke(() =>
        {
            Connections.Clear();
            Count = 0;
        });
    }
}
