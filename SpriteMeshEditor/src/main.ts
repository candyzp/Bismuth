import { Editor } from './editor';
import './style.css'

const canvas = document.querySelector('#canvas') as HTMLCanvasElement;

new Editor(canvas);