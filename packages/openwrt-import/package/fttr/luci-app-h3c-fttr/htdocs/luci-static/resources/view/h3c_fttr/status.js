'use strict';
'require view';
'require form';
'require rpc';
'require poll';
'require ui';

/*
 * LuCI view for the H3C HM2004-DU FMCS FTTR master-OLT FPGA.
 *
 * Two panels:
 *
 *   Status           - driver, device node, control net devices and the FPGA
 *                      interrupt status words, refreshed once a second;
 *   Register console - read and write the FPGA and BOSA register windows.
 *
 * The register console is the point of the page during bring-up: the bulk of
 * the FPGA register map has not been recovered yet (see
 * docs/REVERSE_ENGINEERING.md section 10.2, risk R5), so the practical way to
 * find it is to read registers and watch what changes.
 */

var callStatus = rpc.declare({
	object: 'luci.h3c_fttr',
	method: 'status',
	expect: { }
});

var callRegRead = rpc.declare({
	object: 'luci.h3c_fttr',
	method: 'regread',
	params: [ 'type', 'addr' ],
	expect: { }
});

var callRegWrite = rpc.declare({
	object: 'luci.h3c_fttr',
	method: 'regwrite',
	params: [ 'type', 'addr', 'value' ],
	expect: { }
});

function badge(ok, label) {
	return E('span', {
		'class': 'ifacebadge',
		'style': 'margin-right:.5em; padding:.15em .6em; border-radius:.6em; ' +
			 'background:' + (ok ? '#2e7d32' : '#8e1b1b') + '; color:#fff;'
	}, label);
}

function field(label, cell) {
	return E('div', { 'style': 'margin:.2em 0;' }, [
		E('strong', {}, label + ': '), cell
	]);
}

return view.extend({
	load: function() {
		return callStatus();
	},

	render: function(status) {
		var self = this;
		var cells = {};

		function update(s) {
			var put = function(key, value) {
				if (cells[key])
					cells[key].textContent =
						(value == null || value === '') ? '—' : value;
			};

			put('version', s.driver_version);
			put('holdtimes', s.holdtimes);
			put('irq', s.irq_count);
			put('int0', s.int_status0);
			put('int1', s.int_status1);
			put('ploam', s.ploam_rx + ' / ' + s.ploam_tx);
			put('omci', s.omci_rx + ' / ' + s.omci_tx);

			cells.badges.innerHTML = '';
			cells.badges.appendChild(badge(s.loaded === true, 'driver'));
			cells.badges.appendChild(badge(s.chardev === true, 'fmcs_mci'));
			cells.badges.appendChild(badge(s.netdev_ploam === true, 'molt_ploam'));
			cells.badges.appendChild(badge(s.netdev_omci === true, 'molt_omci'));
		}

		function makeCell(label, key) {
			cells[key] = E('span', {}, '—');

			return field(label, cells[key]);
		}

		var statusPanel = E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, 'FTTR master-OLT FPGA'),
			cells.badges = E('div', { 'style': 'margin-bottom:.6em;' }),
			makeCell('Driver version', 'version'),
			makeCell('Bitstream hold times', 'holdtimes'),
			makeCell('FPGA interrupts', 'irq'),
			makeCell('FPGA interrupt status 0 (0x0000)', 'int0'),
			makeCell('FPGA interrupt status 1 (0x0004)', 'int1'),
			makeCell('molt_ploam rx / tx', 'ploam'),
			makeCell('molt_omci rx / tx', 'omci'),
			E('p', { 'class': 'cbi-section-descr' },
			  'Green badges show which parts of the driver are present. ' +
			  'The page needs the fttr-fmcs module loaded and a device tree ' +
			  'node matching "h3c,fmcs" before any of the register access works. ' +
			  'An interrupt count of zero on a running PON is the first thing to ' +
			  'check when PLOAM traffic is missing.')
		]);

		var console = new form.Map('h3c_fttr', 'Register console',
			'Read and write the FPGA and BOSA register windows through ' +
			'/dev/fmcs_mci. Addresses and values accept decimal or 0x-prefixed ' +
			'hexadecimal.');

		var section = console.section(form.NamedSection, 'console', 'console',
			'Register access');
		section.anonymous = true;

		var readType = section.option(form.ListValue, 'read_type', 'Window',
			'Which address space to access.');
		readType.value('fpga', 'FPGA');
		readType.value('bosa', 'BOSA');
		readType.default = 'fpga';

		var readAddr = section.option(form.Value, 'read_addr', 'Address',
			'Register address, for example 0x0000.');
		readAddr.default = '0x0000';
		readAddr.rmempty = false;

		var readBtn = section.option(form.Button, '_read', ' ',
			'Read the register and show the result below.');
		readBtn.inputtitle = 'Read';
		readBtn.inputstyle = 'apply';
		readBtn.onclick = function() {
			var type = this.map.lookupOption('read_type')[0].formvalue();
			var addr = this.map.lookupOption('read_addr')[0].formvalue();

			return callRegRead(type, addr).then(function(res) {
				if (res && res.error)
					ui.addNotification(null, E('p', {}, res.error), 'error');
				else if (res)
					ui.addNotification(null, E('p', {}, [
						'Read ', type, ' register ', res.addr,
						' = ', String(res.value)
					]), 'info');
				else
					ui.addNotification(null, E('p', {}, 'No response'),
						'error');
			});
		};

		var writeAddr = section.option(form.Value, 'write_addr', 'Address',
			'Register address to write.');
		writeAddr.default = '0x0000';

		var writeValue = section.option(form.Value, 'write_value', 'Value',
			'Value to write.');
		writeValue.default = '0x00000000';

		var writeBtn = section.option(form.Button, '_write', ' ',
			'Writes take effect immediately on the FPGA. Read the register back ' +
			'afterwards to confirm.');
		writeBtn.inputtitle = 'Write';
		writeBtn.inputstyle = 'apply';
		writeBtn.onclick = function() {
			var type = this.map.lookupOption('read_type')[0].formvalue();
			var addr = this.map.lookupOption('write_addr')[0].formvalue();
			var value = this.map.lookupOption('write_value')[0].formvalue();

			return callRegWrite(type, addr, value).then(function(res) {
				if (res && res.error)
					ui.addNotification(null, E('p', {}, res.error), 'error');
				else
					ui.addNotification(null, E('p', {}, [
						'Wrote ', value, ' to ', type, ' register ', addr
					]), 'info');
			});
		};

		update(status);

		poll.add(function() {
			return callStatus().then(update);
		}, 1);

		return console.render().then(function(mapEl) {
			return E('div', {}, [ statusPanel, mapEl ]);
		});
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
