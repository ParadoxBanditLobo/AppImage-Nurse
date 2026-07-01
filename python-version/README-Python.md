Python Instructions:

Requires Python 3 and PySide6.

Setup and run:

Linux / macOS
1. Open a terminal in this folder
2. Create a virtual environment:
   python3 -m venv .venv
3. Activate it:
   source .venv/bin/activate
4. Install dependencies:
   python -m pip install PySide6
5. Run the app:
   python flashcards.py

python3 -m venv .venv
source .venv/bin/activate
python -m pip install PySide6
python flashcards.py

Windows
1. Open Command Prompt in this folder
2. Create a virtual environment:
   py -m venv .venv
3. Activate it:
   .venv\Scripts\activate
4. Install dependencies:
   python -m pip install -r PySide6 
5. Run the app:
   python flashcards.py

py -m venv .venv
.venv\Scripts\activate
python -m pip install PySide6
python flashcards.py

Setup and run:


How to use:

Use some spreadsheet software and export as CSV,
Use the included CSV as a template, 
the question/answer is needed at the top, and add your questions from there.
First line must be question, rest are answers,

Format:

1 question, 1 answer, : will load as a definition card,
1 question, 1 answer, 1wrong, 2wrong, 3wrong, : will load as multiple question cards.
(can have less than 3 wrong answers)

------
Flashcards.pck must be in folder to use.

-------------------------------------------------------------

About:

It is a simple tool, it saves no files or questions when it is closed.
I made this because I couldn't find any tool like it.
That feeling of using flash cards, and going over the ones you missed to learn,
is what I am aiming for.
Hopefully this can help you in your studies.

--------------------------------------------------------------
Notice:

No warranty is provided,
I am not responsible for how the application is used
by end-users.

Built with Redot Engine.

Redot Engine is free and open source software licensed under the MIT License.

Copyright (c) 2024-present Redot Engine contributors
Copyright (c) 2014-present Godot Engine contributors
Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur
