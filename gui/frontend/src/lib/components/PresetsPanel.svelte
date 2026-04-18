<script>
  export let presets = {};
  export let presetName = '';
  export let selected = '';

  export let onSave = () => {};
  export let onLoad = () => {};
  export let onDelete = () => {};

  $: names = Object.keys(presets);
</script>

<details class="panel">
  <summary>Presets</summary>
  <div class="panel-content">
    <div class="form-row">
      <label>Name</label>
      <input type="text" bind:value={presetName} placeholder="e.g. 1kHz sine test">
    </div>
    <div class="panel-actions">
      <button class="btn btn-primary" on:click={onSave}>Save current</button>
    </div>
    {#if names.length > 0}
      <div class="form-row">
        <label>Load</label>
        <select bind:value={selected}>
          <option value="">—</option>
          {#each names as name}
            <option value={name}>{name}</option>
          {/each}
        </select>
      </div>
      <div class="panel-actions">
        <button class="btn btn-primary" on:click={onLoad}
                disabled={!selected}>Load</button>
        <button class="btn btn-danger" on:click={onDelete}
                disabled={!selected}>Delete</button>
      </div>
    {/if}
  </div>
</details>
