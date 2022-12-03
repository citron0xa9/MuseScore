from __future__ import absolute_import, division, print_function, unicode_literals

from threading import Thread, Event
import socketserver
import select
import struct
import json
import functools
import queue
import itertools

import Live, MidiRemoteScript
from _Framework.ControlSurface import ControlSurface

PORT = 29040

'''
class ControlSurface:
    def __init__(self, c_instance):
        pass

    @staticmethod
    def log_message(*args, **kwargs):
        print(*args, **kwargs)
'''


class ReceiveCountTimeoutError(Exception):
    pass


class ReceiveMessageError(Exception):
    pass


class SynchronizeRangeMissingTrackError(Exception):
    pass


class SynchronizeRangeTrackInvalidError(Exception):
    pass


class TCPHandler(socketserver.BaseRequestHandler):
    def __init__(self, request, client_address, server):
        self.muse_score = server.muse_score
        self.received_data = bytearray()
        self.client_connection_closed = False
        self.send_queue = queue.SimpleQueue()
        super().__init__(request, client_address, server)

    def wait_data_ready(self, timeout):
        if self.received_data:
            return True
        (read_ready, write_ready, x_ready) = select.select([self.request], [], [], timeout)
        if read_ready:
            self.received_data = self.request.recv(4096)
            # If we did not receive anything -> client closed connection
            self.client_connection_closed = not self.received_data
        return read_ready

    def handle(self):
        self.request.setblocking(0)
        self.muse_score.log_message("Established connection")
        self.command_loop()
        if self.client_connection_closed:
            self.muse_score.log_message("Connection closed by client")
        else:
            self.muse_score.log_message("Shutting down - closing connection to client")

    def command_loop(self):
        while (not self.muse_score.is_stopping()) and (not self.client_connection_closed):
            if not self.client_connection_closed:
                self.send_queued_messages()
                if self.wait_data_ready(0.1):
                    self.handle_command()

    def send_queued_messages(self):
        while not self.send_queue.empty():
            # There is no other consumer, so get below should not fail
            message = self.send_queue.get_nowait()
            self.send_message(message)

    def send_message(self, message):
        message_bytes = message.encode(encoding="utf-8")
        message_complete = bytearray(struct.pack(">I", len(message_bytes)))
        message_complete.extend(message_bytes)
        self.request.sendall(message_complete)

    def handle_command(self):
        command = None
        try:
            command = self.recv_message()
        except ReceiveMessageError as error:
            self.muse_score.log_message("Error handling command: Reading message failed")
            return

        if command["command"] == "Synchronize":
            callback = functools.partial(self.handle_synchronize, command)
            self.muse_score.schedule_message(0, callback)
        elif command["command"] == "SynchronizeTrackRange":
            callback = functools.partial(self.handle_synchronize_track_range, command)
            self.muse_score.schedule_message(0, callback)

    def handle_synchronize(self, command):
        for track in command["tracks"]:
            with self.muse_score.component_guard():
                self.synchronize_track(track, command["duration"])

    def synchronize_track(self, muse_score_track, score_duration):
        muse_score_track["name"] = muse_score_track["name"].replace("♭", "b")
        ableton_track = None
        existing_tracks = [t for t in self.muse_score.tracks() if t.name == muse_score_track["name"]]
        if existing_tracks:
            ableton_track = existing_tracks[0]
        else:
            ableton_track = self.muse_score.create_midi_track()
            ableton_track.name = muse_score_track["name"]
        clip = self.muse_score.reuse_clip(ableton_track, score_duration)
        if not clip:
            self.muse_score.clear_clips(ableton_track)
            clip = self.muse_score.create_clip(ableton_track, score_duration)

        note_specs, muse_score_ids = self.create_note_specs(muse_score_track["notes"])
        live_ids = clip.add_new_notes(note_specs)
        id_map = dict()
        for muse_score_id, live_id in zip(muse_score_ids, live_ids):
            id_map[muse_score_id] = live_id
        response_msg = json.dumps({"command": "SynchronizeNoteIds", "NoteIdMap": id_map})
        self.send_queue.put(response_msg)

    def handle_synchronize_track_range(self, command):
        for track in command["tracks"]:
            with self.muse_score.component_guard():
                self.synchronize_track_range(track, command["start"], command["duration"], command["score_duration"])

    def synchronize_track_range(self, track_range, start, duration, score_duration):
        track_range["name"] = track_range["name"].replace("♭", "b")
        existing_tracks = [t for t in self.muse_score.tracks() if t.name == track_range["name"]]
        ableton_track = None
        if existing_tracks:
            ableton_track = existing_tracks[0]
        else:
            ableton_track = self.muse_score.create_midi_track()
            ableton_track.name = track_range["name"]
        clip = self.muse_score.get_synchronized_clip(ableton_track)
        if not clip:
            self.muse_score.clear_clips(ableton_track)
            clip = self.muse_score.create_clip(ableton_track, score_duration)

        notes_in_range = self.muse_score.notes_in_range(clip, start, duration)
        note_ids_in_range = {t.note_id for t in notes_in_range}
        self.muse_score.log_message("Range is : " + str(start) + " ; " + str(duration))
        self.muse_score.log_message("Note ids in range is : " + str(note_ids_in_range))
        note_modifications_ids, missing_notes = self.create_note_modifications(track_range["notes"], notes_in_range)
        new_note_specs, new_notes_musescore_ids = self.create_new_notes_spec(track_range["notes"], missing_notes)
        removed_notes_ids = [t for t in note_ids_in_range if t not in note_modifications_ids]
        new_notes_live_ids = self.muse_score.update_note_range(clip, notes_in_range, new_note_specs, removed_notes_ids)

        id_map = dict()
        for muse_score_id, live_id in zip(new_notes_musescore_ids, new_notes_live_ids):
            id_map[muse_score_id] = live_id
        response_msg = json.dumps({"command": "SynchronizeNoteIds", "NoteIdMap": id_map, "ModifiedNotes": note_modifications_ids, "RemovedNotes": removed_notes_ids})
        self.send_queue.put(response_msg)

    def create_note_modifications(self, musescore_notes, live_notes):
        existing_musescore_notes = [t for t in musescore_notes if "liveId" in t]
        live_notes_by_id = {t.note_id: t for t in live_notes}
        self.muse_score.log_message("Live notes is: " + str(live_notes))
        self.muse_score.log_message("Live notes by id is: " + str(live_notes_by_id))
        self.muse_score.log_message("existing notes is: " + str(existing_musescore_notes))
        modified_ids = []
        missing_notes = []
        for note in existing_musescore_notes:
            if note["liveId"] not in live_notes_by_id:
                missing_notes.append(note)
                continue
            live_note = live_notes_by_id[note["liveId"]]
            live_note.pitch = note["pitch"]
            live_note.start_time = note["start_time"]
            live_note.duration = note["duration"]
            live_note.velocity = note["velocity"]
            modified_ids.append(live_note.note_id)
        return modified_ids, missing_notes

    @staticmethod
    def create_new_notes_spec(musescore_notes, missing_notes):
        new_notes = [t for t in musescore_notes if "liveId" not in t]
        new_notes.extend(missing_notes)
        return TCPHandler.create_note_specs(new_notes)

    @staticmethod
    def create_note_specs(notes):
        note_specs = []
        ids = []
        for note in notes:
            note_specs.append(
                Live.Clip.MidiNoteSpecification(note["pitch"], note["start_time"], note["duration"], note["velocity"]))
            ids.append(note["musescoreId"])
        return note_specs, ids

    def recv_message(self):
        try:
            message_size = struct.unpack(">I", self.recv_count(4))[0]
            message = self.recv_count(message_size)
            return json.loads(message)
        except ReceiveCountTimeoutError as error:
            raise ReceiveMessageError

    def recv_count(self, byte_count):
        read_ready = self.wait_data_ready(1.0)
        bytes_received = 0
        data = bytearray()
        while read_ready:
            if self.received_data:
                end_index = min(byte_count - bytes_received, len(self.received_data))
                data.extend(self.received_data[:end_index])
                if end_index < len(self.received_data):
                    self.received_data = self.received_data[end_index:]
                    return data
                self.received_data = bytearray()
                bytes_received += end_index
                if bytes_received == byte_count:
                    return data
                read_ready = self.wait_data_ready(1.0)

        raise ReceiveCountTimeoutError("Reading count bytes timed out")


class BridgeServer(socketserver.ThreadingTCPServer):
    def __init__(self, muse_score):
        super().__init__(("localhost", PORT), TCPHandler)
        self.muse_score = muse_score


class MuseScore(ControlSurface):
    def __init__(self, c_instance):
        ControlSurface.__init__(self, c_instance)
        self.stop_event = Event()
        self.log_message("MuseScore Midi Remote script init!")
        self.bridge_server = BridgeServer(self)
        self.server_thread = Thread(target=self.bridge_server.serve_forever)
        self.server_thread.start()

    def disconnect(self):
        self.stop_event.set()
        self.log_message("MuseScore Midi Remote script disconnect start")
        self.bridge_server.shutdown()
        self.server_thread.join()
        self.log_message("MuseScore Midi Remote script disconnect finished")

    def is_stopping(self):
        return self.stop_event.is_set()

    def tracks(self):
        return self.song().tracks

    def create_midi_track(self):
        return self.song().create_midi_track()

    @staticmethod
    def clear_clips(track):
        for clip in track.arrangement_clips:
            track.delete_clip(clip)

    @staticmethod
    def create_clip(track, duration):
        clip_slot = track.clip_slots[0]
        if clip_slot.has_clip:
            clip_slot.delete_clip()
        clip_slot.create_clip(duration)
        created_clip = track.duplicate_clip_to_arrangement(clip_slot.clip, 0)
        clip_slot.delete_clip()
        return created_clip

    @staticmethod
    def reuse_clip(track, clip_duration):
        if len(track.arrangement_clips) != 1:
            return None
        clip = track.arrangement_clips[0]
        if not clip.is_midi_clip or clip.length != clip_duration or clip.start_time != 0:
            return None
        clip.remove_notes_extended(0, 128, 0, clip_duration)
        return clip

    @staticmethod
    def get_synchronized_clip(track):
        if len(track.arrangement_clips) != 1:
            return None
        clip = track.arrangement_clips[0]
        if not clip.is_midi_clip or clip.start_time != 0:
            return None
        return clip

    @staticmethod
    def notes_in_range(clip, start, duration):
        return clip.get_notes_extended(0, 128, start, duration)

    @staticmethod
    def update_note_range(clip, modifications, new_note_specs, removed_notes_ids):
        clip.apply_note_modifications(modifications)
        clip.remove_notes_by_id(removed_notes_ids)
        return clip.add_new_notes(new_note_specs)


'''
if __name__ == "__main__":
    muse_score = MuseScore(None)
    input("Press Enter to exit...")
    muse_score.disconnect()
'''
